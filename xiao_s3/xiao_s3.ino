/*
 * XIAO ESP32S3 Audio Recorder with ElevenLabs Speech-to-Text
 * Fixed version addressing WiFi connection and file processing issues

 */

#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include <Arduino.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <esp_now.h>
#include <esp_wifi.h>

// WiFi credentials
const char* ssid = "YOUR_SSID";
const char* password = "SSID_PASSWORD";

// ElevenLabs API configuration
const char* elevenlabs_api_key = "ELEVENLABS_API_KEY";
const char* elevenlabs_stt_url = "https://api.elevenlabs.io/v1/speech-to-text";
uint8_t matouch_mac[] = { 0x84, 0xFC, 0xE6, 0x73, 0x1B, 0x0C };

// Audio recording settings
#define RECORD_TIME 5
#define WAV_FILE_NAME "recording"
#define SAMPLE_RATE 16000U
#define SAMPLE_BITS 16
#define WAV_HEADER_SIZE 44
#define VOLUME_GAIN 2

// ESP-NOW settings
#define MAX_RETRIES 3
#define ESPNOW_DELAY 200
#define MODE_SWITCH_DELAY 500

// I2S PDM Configuration for XIAO ESP32S3 built-in microphone
#define I2S_NUM         I2S_NUM_0
#define PDM_CLK_GPIO    (gpio_num_t)42  // CLK pin
#define PDM_DIN_GPIO    (gpio_num_t)41  // DATA pin

#define BUTTON_PIN D1
bool lastButtonState = HIGH;
bool isPressed = false;

// I2S handle
i2s_chan_handle_t rx_handle = NULL;

// Global variables
bool recording_active = false;
bool new_recording_available = false;
String last_transcription = "";
bool esp_now_ready = false;
bool wifi_connected = false;
int successful_espnow_sends = 0;
int failed_espnow_sends = 0;
uint8_t wifi_channel = 1; // Dynamic WiFi channel detection
String current_recording_file = ""; // Track current recording file

// ===== FUNCTION DECLARATIONS =====
void OnDataSent(const uint8_t* mac_addr, esp_now_send_status_t status);
void initializeESPNOW();
void testESPNOWConnection();
bool connectToWiFi();
void record_and_process();
void record_wav();
void process_recording();
void sendTranscriptionViaESPNOW(String transcription);
void setWiFiChannel(uint8_t channel);
bool verifyChannel(uint8_t expected_channel);
String send_to_elevenlabs_stt(String filename);
void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate);
String get_last_transcription();
bool is_recording();
void printESPNOWStats();
bool init_i2s_pdm();
void deinit_i2s_pdm();
void cleanupOldRecordings();

// ===== IMPLEMENTATION =====

bool init_i2s_pdm() {
  Serial.println("Initializing I2S PDM for built-in microphone...");
  
  // I2S channel configuration
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  
  esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  if (ret != ESP_OK) {
    Serial.printf("Failed to create I2S channel: %s\n", esp_err_to_name(ret));
    return false;
  }

  // I2S PDM RX configuration
  i2s_pdm_rx_config_t pdm_rx_cfg = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk = PDM_CLK_GPIO,
      .din = PDM_DIN_GPIO,
      .invert_flags = {
        .clk_inv = false,
      },
    },
  };

  ret = i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg);
  if (ret != ESP_OK) {
    Serial.printf("Failed to initialize PDM RX mode: %s\n", esp_err_to_name(ret));
    return false;
  }

  ret = i2s_channel_enable(rx_handle);
  if (ret != ESP_OK) {
    Serial.printf("Failed to enable I2S channel: %s\n", esp_err_to_name(ret));
    return false;
  }

  Serial.println("I2S PDM initialized successfully");
  return true;
}

void deinit_i2s_pdm() {
  if (rx_handle != NULL) {
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    rx_handle = NULL;
  }
}

// ESP-NOW send callback with better tracking
void OnDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
  Serial.print("ESP-NOW Send to ");
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X ", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("SUCCESS");
    successful_espnow_sends++;
  } else {
    Serial.println("FAILED");
    failed_espnow_sends++;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP); // For D1 button

  while (!Serial)
    ;

  Serial.println("XIAO ESP32S3 Audio Recorder with ElevenLabs STT - Fixed Version");

  // Initialize I2S PDM for built-in microphone
  if (!init_i2s_pdm()) {
    Serial.println("Failed to initialize I2S PDM!");
    while (1)
      ;
  }

  // Initialize SD card
  if (!SD.begin(21)) {
    Serial.println("Failed to mount SD Card!");
    while (1)
      ;
  }
  Serial.println("SD Card initialized successfully");

  // Clean up old recordings to free space
  cleanupOldRecordings();

  // Connect to WiFi first to discover the channel
  if (connectToWiFi()) {
    // Get the actual WiFi channel
    uint8_t primary;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    wifi_channel = primary;
    Serial.printf("WiFi connected on channel: %d\n", wifi_channel);
    
    // Initialize ESP-NOW on the same channel as WiFi
    initializeESPNOW();
  } else {
    Serial.println("Continuing without WiFi - ESP-NOW only mode");
    wifi_channel = 1; // Default channel
    initializeESPNOW();
  }

  delay(500);
}

void loop() {
  bool currentState = digitalRead(BUTTON_PIN) == LOW;

  if (currentState && !isPressed) {
    // Button just pressed
    isPressed = true;
    Serial.println("Button pressed. Starting recording...");
    record_wav_streaming();  // New function below
    process_recording();
  }

  if (!currentState && isPressed) {
    // Button just released (recording already handled inside function)
    isPressed = false;
    Serial.println("Button released.");
  }

  delay(50); // Debounce
}
void setWiFiChannel(uint8_t channel) {
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  delay(ESPNOW_DELAY);
}

bool verifyChannel(uint8_t expected_channel) {
  uint8_t primary;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&primary, &second);

  Serial.printf("Current channel: %d, Expected: %d\n", primary, expected_channel);
  return (primary == expected_channel);
}

void initializeESPNOW() {
  Serial.println("Initializing ESP-NOW...");

  // Ensure we're in STA mode
  WiFi.mode(WIFI_STA);
  delay(MODE_SWITCH_DELAY);

  // Set channel to match WiFi
  setWiFiChannel(wifi_channel);

  if (!verifyChannel(wifi_channel)) {
    Serial.println("Failed to set correct channel!");
    return;
  }

  Serial.print("XIAO MAC Address: ");
  Serial.println(WiFi.macAddress());

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  // Register send callback
  esp_now_register_send_cb(OnDataSent);

  // Add peer with proper error handling
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, matouch_mac, 6);
  peerInfo.channel = wifi_channel;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  esp_err_t addStatus = esp_now_add_peer(&peerInfo);
  if (addStatus != ESP_OK) {
    Serial.printf("Failed to add peer: %d\n", addStatus);
    return;
  }

  esp_now_ready = true;
  Serial.printf("ESP-NOW initialized successfully on channel %d\n", wifi_channel);

  // Test connection
  testESPNOWConnection();
}

void testESPNOWConnection() {
  Serial.println("Testing ESP-NOW connection...");
  String test_msg = "XIAO_TEST_" + String(millis());

  // Try sending once first
  if (!verifyChannel(wifi_channel)) {
    setWiFiChannel(wifi_channel);
  }

  esp_err_t result = esp_now_send(matouch_mac, (uint8_t*)test_msg.c_str(), test_msg.length() + 1);
  Serial.printf("Test 1: %s\n", (result == ESP_OK) ? "Queued" : "Failed");
  
  // Only retry if the first attempt failed
  if (result != ESP_OK) {
    Serial.println("First test failed, retrying...");
    
    for (int i = 1; i < MAX_RETRIES; i++) {  // Start from 1 since we already tried once
      delay(1000); // Wait before retry
      
      if (!verifyChannel(wifi_channel)) {
        setWiFiChannel(wifi_channel);
      }

      result = esp_now_send(matouch_mac, (uint8_t*)test_msg.c_str(), test_msg.length() + 1);
      Serial.printf("Test %d: %s\n", i + 1, (result == ESP_OK) ? "Queued" : "Failed");
      
      // Break if successful
      if (result == ESP_OK) {
        Serial.println("ESP-NOW test successful!");
        break;
      }
    }
  } else {
    Serial.println("ESP-NOW test successful on first try!");
  }
  
  delay(1000);  // Wait for callback
}

bool connectToWiFi() {
  Serial.println("Connecting to WiFi...");

  // Start fresh
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  delay(1000);

  // Try connecting with increased timeout
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) { // Increased timeout
    delay(500);
    Serial.print(".");
    attempts++;
    
    // Check for different failure states
    if (attempts % 20 == 0) {
      Serial.printf("\nWiFi Status: %d, Attempt: %d\n", WiFi.status(), attempts);
      
      // Try reconnecting if stuck
      if (WiFi.status() == WL_CONNECT_FAILED || WiFi.status() == WL_NO_SSID_AVAIL) {
        Serial.println("Retrying WiFi connection...");
        WiFi.disconnect();
        delay(1000);
        WiFi.begin(ssid, password);
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected successfully!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal strength (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    wifi_connected = true;
    return true;
  } else {
    Serial.println("\nFailed to connect to WiFi");
    Serial.printf("Final status: %d\n", WiFi.status());
    wifi_connected = false;
    return false;
  }
}

void record_and_process() {
  record_wav();
  if (!current_recording_file.isEmpty()) {
    process_recording();
  }
}
void record_wav_streaming() {
  if (rx_handle == NULL) {
    Serial.println("I2S not initialized!");
    return;
  }

  const uint32_t max_record_time = 15; // safety timeout in seconds
  const uint32_t max_bytes = SAMPLE_RATE * SAMPLE_BITS / 8 * max_record_time;

  String filename = "/" + String(WAV_FILE_NAME) + "_" + String(millis()) + ".wav";
  current_recording_file = filename;

  File file = SD.open(filename.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file!");
    current_recording_file = "";
    return;
  }

  uint8_t wav_header[WAV_HEADER_SIZE];
  generate_wav_header(wav_header, 0, SAMPLE_RATE);  // temp 0 size
  file.write(wav_header, WAV_HEADER_SIZE);

  uint8_t* buffer = (uint8_t*)malloc(512);
  if (!buffer) {
    Serial.println("Failed to allocate buffer!");
    file.close();
    return;
  }

  recording_active = true;
  size_t total_bytes = 0;
  unsigned long startTime = millis();

  Serial.println("Recording...");

  while (digitalRead(BUTTON_PIN) == LOW && (millis() - startTime < max_record_time * 1000)) {
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_handle, buffer, 512, &bytes_read, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) continue;

    // Apply volume gain
    for (size_t i = 0; i < bytes_read; i += 2) {
      int16_t* sample = (int16_t*)&buffer[i];
      int32_t amp = (*sample) << VOLUME_GAIN;
      if (amp > 32767) amp = 32767;
      if (amp < -32768) amp = -32768;
      *sample = (int16_t)amp;
    }

    file.write(buffer, bytes_read);
    total_bytes += bytes_read;
  }

  recording_active = false;
  free(buffer);

  // Rewrite WAV header with actual size
  file.seek(0);
  generate_wav_header(wav_header, total_bytes, SAMPLE_RATE);
  file.write(wav_header, WAV_HEADER_SIZE);
  file.close();

  Serial.printf("Recording finished. File saved: %s (%d bytes)\n", filename.c_str(), total_bytes);
}


void process_recording() {
  if (current_recording_file.isEmpty()) {
    Serial.println("No current recording file!");
    return;
  }

  Serial.printf("Processing file: %s\n", current_recording_file.c_str());

  String transcription = send_to_elevenlabs_stt(current_recording_file);

  if (!transcription.isEmpty()) {
    Serial.println("Transcription result:");
    Serial.println(transcription);
    last_transcription = transcription;

    // Send via ESP-NOW regardless of WiFi status
    sendTranscriptionViaESPNOW(transcription);
  } else {
    Serial.println("Failed to get transcription - sending raw audio info via ESP-NOW");
    String fallback_msg = "AUDIO_RECORDED_" + String(millis());
    sendTranscriptionViaESPNOW(fallback_msg);
  }
  
  // Clear current recording file after processing
  current_recording_file = "";
}

void sendTranscriptionViaESPNOW(String transcription) {
  if (!esp_now_ready) {
    Serial.println("ESP-NOW not ready");
    return;
  }

  // If WiFi is connected, disconnect temporarily for ESP-NOW
  bool was_wifi_connected = (WiFi.status() == WL_CONNECTED);
  if (was_wifi_connected) {
    WiFi.disconnect(false, false);
    delay(MODE_SWITCH_DELAY);
  }

  // Ensure correct channel
  setWiFiChannel(wifi_channel);

  if (!verifyChannel(wifi_channel)) {
    Serial.println("Channel verification failed!");
    return;
  }

  // Limit message size
  if (transcription.length() > 249) {
    transcription = transcription.substring(0, 249);
  }

  Serial.printf("Sending transcription via ESP-NOW (length: %d)\n", transcription.length());
  Serial.printf("Target MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                matouch_mac[0], matouch_mac[1], matouch_mac[2],
                matouch_mac[3], matouch_mac[4], matouch_mac[5]);

  bool sent_successfully = false;

  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
    Serial.printf("Send attempt %d/%d\n", attempt + 1, MAX_RETRIES);

    // Double-check channel before each attempt
    if (!verifyChannel(wifi_channel)) {
      setWiFiChannel(wifi_channel);
    }

    esp_err_t result = esp_now_send(matouch_mac, (uint8_t*)transcription.c_str(), transcription.length() + 1);

    if (result == ESP_OK) {
      Serial.println("Message queued successfully");
      delay(1000);  // Wait for callback
      sent_successfully = true;
      break;
    } else {
      Serial.printf("Failed to queue message: %d - ", result);

      switch (result) {
        case ESP_ERR_ESPNOW_NOT_INIT:
          Serial.println("ESP-NOW not initialized");
          break;
        case ESP_ERR_ESPNOW_ARG:
          Serial.println("Invalid argument");
          break;
        case ESP_ERR_ESPNOW_INTERNAL:
          Serial.println("Internal error");
          break;
        case ESP_ERR_ESPNOW_NO_MEM:
          Serial.println("Out of memory");
          break;
        case ESP_ERR_ESPNOW_NOT_FOUND:
          Serial.println("Peer not found");
          break;
        case ESP_ERR_ESPNOW_IF:
          Serial.println("Interface error");
          break;
        default:
          Serial.printf("Unknown error: %d\n", result);
      }

      delay(500);
    }
  }

  // Reconnect to WiFi if it was connected before
  if (was_wifi_connected && wifi_connected) {
    delay(MODE_SWITCH_DELAY);
    Serial.println("Reconnecting to WiFi...");
    connectToWiFi();
  }
}

void printESPNOWStats() {
  Serial.println("=== ESP-NOW Statistics ===");
  Serial.printf("Successful sends: %d\n", successful_espnow_sends);
  Serial.printf("Failed sends: %d\n", failed_espnow_sends);

  if (successful_espnow_sends + failed_espnow_sends > 0) {
    float success_rate = (float)successful_espnow_sends / (successful_espnow_sends + failed_espnow_sends) * 100;
    Serial.printf("Success rate: %.1f%%\n", success_rate);
  }

  Serial.printf("WiFi Status: %s\n", wifi_connected ? "Connected" : "Disconnected");
  Serial.printf("Current Channel: %d\n", wifi_channel);
  Serial.println("=========================");
}

void cleanupOldRecordings() {
  Serial.println("Cleaning up old recordings...");
  
  File root = SD.open("/");
  if (!root) {
    Serial.println("Failed to open root directory");
    return;
  }

  int deleted_count = 0;
  File file = root.openNextFile();
  while (file) {
    String filename = file.name();
    if (filename.startsWith(WAV_FILE_NAME) && filename.endsWith(".wav")) {
      file.close();
      if (SD.remove("/" + filename)) {
        deleted_count++;
        Serial.printf("Deleted old recording: %s\n", filename.c_str());
      }
    } else {
      file.close();
    }
    file = root.openNextFile();
  }
  
  root.close();
  Serial.printf("Cleaned up %d old recordings\n", deleted_count);
}

String send_to_elevenlabs_stt(String filename) {
  if (!wifi_connected || WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected - skipping STT API call");
    return "";
  }

  File file = SD.open(filename.c_str());
  if (!file) {
    Serial.println("Failed to open audio file!");
    return "";
  }

  size_t file_size = file.size();
  Serial.printf("File size: %d bytes\n", file_size);

  if (file_size > 500000) {
    Serial.println("File too large, skipping...");
    file.close();
    return "";
  }

  uint8_t* audio_data = (uint8_t*)malloc(file_size);
  if (!audio_data) {
    Serial.println("Failed to allocate memory for audio data!");
    file.close();
    return "";
  }

  file.read(audio_data, file_size);
  file.close();

  HTTPClient http;
  http.setTimeout(30000);
  http.setConnectTimeout(10000);

  if (!http.begin(elevenlabs_stt_url)) {
    Serial.println("Failed to begin HTTP connection!");
    free(audio_data);
    return "";
  }

  http.addHeader("xi-api-key", elevenlabs_api_key);

  String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  String body_start = "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"model_id\"\r\n\r\n";
  body_start += "scribe_v1\r\n";
  body_start += "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  body_start += "Content-Type: audio/wav\r\n\r\n";

  String body_end = "\r\n--" + boundary + "--\r\n";

  size_t total_size = body_start.length() + file_size + body_end.length();

  uint8_t* complete_body = (uint8_t*)malloc(total_size);
  if (!complete_body) {
    Serial.println("Failed to allocate memory for complete body!");
    free(audio_data);
    http.end();
    return "";
  }

  memcpy(complete_body, body_start.c_str(), body_start.length());
  memcpy(complete_body + body_start.length(), audio_data, file_size);
  memcpy(complete_body + body_start.length() + file_size, body_end.c_str(), body_end.length());

  free(audio_data);

  Serial.println("Sending request to ElevenLabs...");

  int httpResponseCode = http.POST(complete_body, total_size);

  free(complete_body);

  String response = "";

  if (httpResponseCode > 0) {
    Serial.printf("HTTP Response code: %d\n", httpResponseCode);
    response = http.getString();

    if (httpResponseCode == 200) {
      Serial.println("Success! Response received:");
      Serial.println(response);

      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, response);

      if (error) {
        Serial.printf("JSON parsing failed: %s\n", error.c_str());
        http.end();
        return "";
      }

      if (doc.containsKey("text")) {
        String transcription = doc["text"].as<String>();
        http.end();
        return transcription;
      } else {
        Serial.println("No text field in response");
      }
    } else {
      Serial.println("HTTP Error. Response:");
      Serial.println(response);
    }
  } else {
    Serial.printf("HTTP request failed with error: %d\n", httpResponseCode);
  }

  http.end();
  return "";
}

void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate) {
  uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
  uint32_t byte_rate = sample_rate * SAMPLE_BITS / 8;

  const uint8_t set_wav_header[] = {
    'R',
    'I',
    'F',
    'F',
    file_size,
    file_size >> 8,
    file_size >> 16,
    file_size >> 24,
    'W',
    'A',
    'V',
    'E',
    'f',
    'm',
    't',
    ' ',
    0x10,
    0x00,
    0x00,
    0x00,
    0x01,
    0x00,
    0x01,
    0x00,
    sample_rate,
    sample_rate >> 8,
    sample_rate >> 16,
    sample_rate >> 24,
    byte_rate,
    byte_rate >> 8,
    byte_rate >> 16,
    byte_rate >> 24,
    0x02,
    0x00,
    0x10,
    0x00,
    'd',
    'a',
    't',
    'a',
    wav_size,
    wav_size >> 8,
    wav_size >> 16,
    wav_size >> 24,
  };
  memcpy(wav_header, set_wav_header, sizeof(set_wav_header));
}

String get_last_transcription() {
  return last_transcription;
}

bool is_recording() {
  return recording_active;
}