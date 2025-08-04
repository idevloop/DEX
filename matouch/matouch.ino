#include <Arduino_GFX_Library.h>
#include <JPEGDecoder.h>
#include "SD_MMC.h"
#include "bb_captouch.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Audio.h>

// --- Display Pins ---
#define TFT_BLK 48
#define TFT_RES -1
#define TFT_CS 15
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCLK 14
#define TFT_DC 21

// --- Touch Pins ---
#define TOUCH_SDA 39
#define TOUCH_SCL 38
#define TOUCH_RST 1
#define TOUCH_INT 40

// --- SD_MMC Pins ---
#define PIN_SD_CLK 42
#define PIN_SD_CMD 2
#define PIN_SD_D0 41

// --- Audio Pins ---
#define pin_I2S_BCLK 6  // I2S Bit Clock
#define pin_I2S_LRC 5   // I2S Left/Right Clock (Word Select)
#define pin_I2S_DOUT 7  // I2S Data Output

// --- WiFi Configuration ---
const char* ssid = "YOUR_SSID";         // Replace with your WiFi SSID
const char* password = "SSID_PASSWORD";  // Replace with your WiFi password
const char* OPENAI_API_KEY = "OPENAI_API_KEY";

// --- Audio Settings ---
#define INITIAL_VOLUME 21  // Volume 0-21
#define TTS_VOICE "onyx"   // OpenAI voice: alloy|ash|coral|echo|fable|onyx|nova|sage|shimmer

// --- ESP-NOW Settings ---
#define WIFI_CHANNEL 1  // Same as sender
#define MAX_MESSAGE_SIZE 250

// --- Debug Configuration ---
#ifndef DEBUG
#define DEBUG true
#define DebugPrint(x) \
  if (DEBUG) { Serial.print(x); }
#define DebugPrintln(x) \
  if (DEBUG) { Serial.println(x); }
#endif

// --- Display Init ---
Arduino_ESP32SPI* bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO, HSPI, true);
Arduino_GFX* gfx = new Arduino_ST7789(bus, TFT_RES, 0, true);  // 240x320

BBCapTouch touch;
Audio audio_player;

// Screen states
enum ScreenState {
  SCREEN_HOME,
  SCREEN_MENU,
  SCREEN_BURGERS,
  SCREEN_SIDES,
  SCREEN_DRINKS,
  SCREEN_DESSERTS
};

ScreenState currentScreen = SCREEN_HOME;
bool touchActive = false;
unsigned long lastTouchTime = 0;
const unsigned long TOUCH_DEBOUNCE = 100;


// Display dimensions
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

// Touch region structure
struct TouchRegion {
  int x1, y1, x2, y2;
  const char* name;
};

// Menu screen touch regions (2x2 grid)
TouchRegion menuRegions[] = {
  { 10, 10, 120, 140, "BURGERS" },
  { 130, 10, 240, 140, "SIDES" },
  { 10, 150, 120, 280, "DRINKS" },
  { 130, 150, 240, 280, "DESSERTS" }
};

// Category screen touch regions (back button + 2 items)
TouchRegion categoryRegions[] = {
  { 190, 10, 240, 40, "BACK" },
  { 10, 40, 200, 165, "ITEM1" },
  { 10, 170, 240, 295, "ITEM2" }
};

bool displayingAIResponse = false;
unsigned long aiResponseStartTime = 0;
const unsigned long AI_RESPONSE_DISPLAY_TIME = 6000;  // Show AI response for 6 seconds
String currentAIResponse = "";

// --- OpenAI Integration ---
#define TIMEOUT_OPEN_AI 15
#define MESSAGES_SIZE 100000

String MESSAGES =
  "{\"role\": \"system\", \"content\": "
  "\"You are Dex, a friendly and helpful AI assistant working at our restaurant. "
  "Your job is to assist customers by providing information about our exclusive 8-item menu only. "
  "Our menu consists of: "
  "BURGERS: (1) Aloo Tikki Burger, (2) Classic Chicken Burger | "
  "SIDES: (1) French Fries, (2) Chilli Garlic Naan | "
  "DRINKS: (1) Coca Cola, (2) Iced Mocha | "
  "DESSERTS: (1) Vanilla Soft Serve Cone, (2) Oreo Cookie McFlurry. "
  "When customers ask about menu items, focus on taste, ingredients, and calories. "
  "Provide helpful suggestions from our menu based on customer preferences. "
  "Do NOT mention any items not on our menu. Keep responses warm, conversational, and under 3 sentences. "
  "Keep answers short, informative don't expand too much. "
  "Help customers make the best choice from our available options.\"}";

// Menu item prompts for AI - McDonald's specific items
const char* menuPrompts[] = {
  // Burgers
  "A customer selected the Aloo Tikki Burger. Tell them about this delicious vegetarian burger with spiced potato patty, fresh lettuce, tomatoes, and creamy mayo - approximately 370 calories of Indian-inspired goodness!",
  
  "A customer selected the Classic Chicken Burger. Describe this juicy grilled chicken breast with crisp lettuce, tomatoes, and special sauce in a soft bun - around 540 calories of pure satisfaction!",
  
  // Sides
  "A customer selected French Fries. Tell them about these golden, crispy potato fries seasoned with salt - approximately 320 calories of the perfect crunchy side that pairs with everything!",
  
  "A customer selected Chilli Garlic Naan. Describe this warm, soft Indian bread infused with spicy chilli and aromatic garlic - around 280 calories of flavorful fusion that adds an exciting twist to any meal!",
  
  // Drinks
  "A customer selected Coca Cola. Tell them about this classic refreshing cola with its signature taste and fizzy satisfaction - 140 calories of the perfect meal companion!",
  
  "A customer selected Iced Mocha. Describe this rich blend of espresso, chocolate, and cold milk topped with whipped cream - approximately 290 calories of coffee shop indulgence!",
  
  // Desserts
  "A customer selected the Vanilla Soft Serve Cone. Tell them about this creamy, smooth vanilla ice cream in a crispy cone - around 200 calories of classic sweet perfection!",
  
  "A customer selected the Oreo Cookie McFlurry. Describe this creamy vanilla soft serve mixed with crunchy Oreo cookie pieces - approximately 510 calories of cookie-licious heaven!"
};

// WiFi and AI status
bool wifiConnected = false;
bool aiProcessing = false;

String currentUserQuestion = "";  // Store the current user's question

// ESP-NOW variables
char receivedMessage[MAX_MESSAGE_SIZE + 1];
bool newMessageReceived = false;
unsigned long lastMessageTime = 0;
int messagesReceived = 0;
int messagesProcessed = 0;

#define MAX_MESSAGES_QUEUE 5
struct MessageQueue {
  char messages[MAX_MESSAGES_QUEUE][MAX_MESSAGE_SIZE + 1];
  int head = 0;
  int tail = 0;
  int count = 0;
};

MessageQueue messageQueue;

// TTS playback state
bool isSpeaking = false;
bool pendingTTS = false;
String pendingMessage = "";

// ESP-NOW callback function
// === FIXED ESP-NOW CALLBACK ===
void onDataRecv(const esp_now_recv_info* recv_info, const uint8_t* data, int len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
           recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);

  Serial.println("\n=== ESP-NOW MESSAGE RECEIVED ===");
  Serial.print("From MAC: ");
  Serial.println(macStr);
  Serial.printf("Data length: %d bytes\n", len);

  // Check if queue is full
  if (messageQueue.count >= MAX_MESSAGES_QUEUE) {
    Serial.println("Message queue full! Dropping oldest message");
    // Remove oldest message
    messageQueue.tail = (messageQueue.tail + 1) % MAX_MESSAGES_QUEUE;
    messageQueue.count--;
  }

  // Clear the buffer
  memset(messageQueue.messages[messageQueue.head], 0, sizeof(messageQueue.messages[messageQueue.head]));

  // Copy received data (ensure it fits in buffer)
  int copyLen = min(len, MAX_MESSAGE_SIZE);
  memcpy(messageQueue.messages[messageQueue.head], data, copyLen);
  messageQueue.messages[messageQueue.head][copyLen] = '\0';  // Ensure null termination

  Serial.print("Message: \"");
  Serial.print(messageQueue.messages[messageQueue.head]);
  Serial.println("\"");

  // Update queue
  messageQueue.head = (messageQueue.head + 1) % MAX_MESSAGES_QUEUE;
  messageQueue.count++;

  newMessageReceived = true;
  lastMessageTime = millis();
  messagesReceived++;

  Serial.printf("Messages in queue: %d/%d\n", messageQueue.count, MAX_MESSAGES_QUEUE);
  Serial.println("=== MESSAGE QUEUED FOR AI PROCESSING ===\n");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Starting Enhanced Food Menu System with AI & TTS ===");

  // Init display
  gfx->begin();
  gfx->setRotation(0);
  gfx->fillScreen(BLACK);

  // Set backlight
  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  Serial.println("Display initialized");

  // Show initialization screen
  showInitializationScreen();

  // Init WiFi
  initWiFi();

  // Init Audio System
  Serial.println("Initializing Audio System...");
  audio_player.setPinout(pin_I2S_BCLK, pin_I2S_LRC, pin_I2S_DOUT);
  audio_player.setVolume(INITIAL_VOLUME);
  Serial.println("Audio system initialized");

  // Test TTS with a welcome message
  Serial.println("Testing TTS with welcome message...");
  playTextToSpeech("Welcome to restaurant! I'm your AI assistant. I can help you explore our menu and answer any questions you have about our delicious food options.");

  // Wait a bit for TTS to start, then continue initialization
  delay(2000);

  // Init touch
  Serial.println("Initializing touch...");
  touch.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
  Serial.println("Touch initialization attempted");

  delay(500);
  TOUCHINFO ti;
  if (touch.getSamples(&ti)) {
    Serial.println("Touch is responding");
  } else {
    Serial.println("Touch may not be responding, but continuing...");
  }

  // Init SD card
  Serial.println("Initializing SD card...");
  if (!SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0)) {
    Serial.println("SD pin setup failed!");
    showError("SD Pin Setup Failed");
    return;
  }

  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_52M, 5)) {
    Serial.println("High speed SD init failed, trying default speed...");
    if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) {
      Serial.println("SD begin failed!");
      showError("SD Init Failed");
      return;
    }
  }
  Serial.println("SD card initialized successfully");

  // List files for debugging
  listSDFiles();

  // Validate required image files
  if (!validateRequiredFiles()) {
    showError("Missing required images");
    return;
  }

  // Initialize ESP-NOW
  initESPNOW();

  Serial.println("All systems ready, displaying home screen...");
  displayHomeScreen();
}
// === FIXED MAIN LOOP ===
void loop() {
  // Always call audio loop to handle TTS playback
  audio_player.loop();

  // Check if currently speaking
  if (isSpeaking && !audio_player.isRunning()) {
    isSpeaking = false;
    Serial.println("TTS playback completed");

    // Check if there's a pending message
    if (pendingTTS) {
      Serial.println("Playing pending TTS message...");
      playTextToSpeech(pendingMessage);
      pendingTTS = false;
      pendingMessage = "";
    }
  }

  // Handle AI response auto-dismiss (only if user hasn't interacted)
  if (displayingAIResponse && !isSpeaking && (millis() - aiResponseStartTime) > AI_RESPONSE_DISPLAY_TIME) {
    Serial.println("Auto-dismissing AI response after timeout");
    dismissAIResponse();
  }

  // CRITICAL FIX: Handle ESP-NOW messages continuously
  handleESPNOWMessages();

  // CRITICAL FIX: Always handle touch input (with conditions inside the function)
  handleTouchInput();

  delay(10);
}
// === UPDATED MESSAGE HANDLER ===
void handleESPNOWMessages() {
  // Process one message at a time, even if AI is not processing
  if (messageQueue.count > 0 && !isSpeaking) {
    // Get the next message from queue
    String userQuestion = String(messageQueue.messages[messageQueue.tail]);

    // Remove message from queue
    messageQueue.tail = (messageQueue.tail + 1) % MAX_MESSAGES_QUEUE;
    messageQueue.count--;

    messagesProcessed++;

    Serial.println("\n=== PROCESSING QUEUED MESSAGE ===");
    Serial.print("User Question: \"");
    Serial.print(userQuestion);
    Serial.println("\"");
    Serial.printf("Length: %d characters\n", userQuestion.length());
    Serial.printf("Messages remaining in queue: %d\n", messageQueue.count);

    // Process the question with AI if it's not empty
    if (userQuestion.length() > 0) {
      Serial.println("Processing question with AI...");

      // STORE THE USER QUESTION FOR DISPLAY
      currentUserQuestion = userQuestion;

      // Show AI processing on screen (non-blocking)
      showAIStatus("Processing question...", CYAN, 0);

      // CRITICAL FIX: Set aiProcessing flag here
      aiProcessing = true;

      // Get AI response
      bool useWebSearch = containsTimeRelatedWords(userQuestion);
      String aiResponse = Open_AI(userQuestion, OPENAI_API_KEY, useWebSearch, "Mumbai");

      // CRITICAL FIX: Reset aiProcessing flag here
      aiProcessing = false;

      if (aiResponse.length() > 0) {
        Serial.println("AI Response received:");
        Serial.println("AI: " + aiResponse);

        // Clean the response for better TTS
        aiResponse = cleanResponseForTTS(aiResponse);

        // Show AI response on screen WITH THE USER QUESTION
        showAIResponseWithQuestion(currentUserQuestion, aiResponse);

        // Speak the AI response
        playTextToSpeech(aiResponse);

      } else {
        Serial.println("No AI response received");
        playTextToSpeech("I'm sorry, I couldn't process your question right now. Please try again.");
      }
    } else {
      Serial.println("Empty message received, skipping AI processing");
    }

    printStatistics();
  }
}

// === FIXED TOUCH HANDLER ===
void handleTouchInput() {
  TOUCHINFO ti;

  if (touch.getSamples(&ti) && ti.count > 0) {
    unsigned long currentTime = millis();

    if (!touchActive && (currentTime - lastTouchTime) > TOUCH_DEBOUNCE) {
      int x = ti.x[0];
      int y = ti.y[0];

      Serial.printf("Touch at (%d, %d) on screen %d\n", x, y, currentScreen);

      // CRITICAL FIX: Allow touch even during AI processing
      // Only block touch if actively speaking and displaying AI response
      if (displayingAIResponse && isSpeaking) {
        Serial.println("Dismissing AI response due to touch during speech");
        dismissAIResponse();
      } else {
        handleTouch(x, y);
      }

      touchActive = true;
      lastTouchTime = currentTime;
    }
  } else {
    touchActive = false;
  }
}


void initWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected successfully!");
    Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
  } else {
    wifiConnected = false;
    Serial.println("\nWiFi connection failed!");
  }
}

void initESPNOW() {
  // Initialize ESP-NOW
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.print("MaTouch MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("WiFi Channel: %d\n", WIFI_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  } else {
    Serial.println("ESP-NOW initialized successfully");
  }

  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP-NOW receive callback registered");
  Serial.println("\nReady to receive questions via ESP-NOW and provide AI answers...");
}

// Function to check if the question might need web search
bool containsTimeRelatedWords(String text) {
  text.toLowerCase();
  return (text.indexOf("today") >= 0 || text.indexOf("tomorrow") >= 0 || text.indexOf("weather") >= 0 || text.indexOf("news") >= 0 || text.indexOf("current") >= 0 || text.indexOf("latest") >= 0);
}

// Function to clean AI response for better TTS output
String cleanResponseForTTS(String response) {
  // Remove markdown formatting
  response.replace("**", "");
  response.replace("*", "");
  response.replace("_", "");

  // Remove URLs (basic cleanup)
  int urlStart = response.indexOf("http");
  while (urlStart >= 0) {
    int urlEnd = response.indexOf(" ", urlStart);
    if (urlEnd == -1) urlEnd = response.length();
    response.remove(urlStart, urlEnd - urlStart);
    urlStart = response.indexOf("http");
  }

  // Clean up extra spaces and newlines
  response.replace("\n\n", ". ");
  response.replace("\n", ". ");
  response.trim();

  return response;
}

void playTextToSpeech(String text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Error: WiFi not connected!");
    aiProcessing = false;
    return;
  }

  if (text.length() == 0) {
    Serial.println("Error: Empty text!");
    aiProcessing = false;
    return;
  }

  if (text.length() > 4000) {
    Serial.println("Warning: Text too long, truncating to 4000 characters");
    text = text.substring(0, 4000);
  }

  Serial.println("=== STARTING TTS PLAYBACK ===");
  Serial.print("Speaking: \"");
  Serial.print(text);
  Serial.println("\"");
  Serial.println("Converting to speech with OpenAI TTS...");
  Serial.println("Voice: " + String(TTS_VOICE));

  // Use the correct method for OpenAI TTS
  audio_player.openai_speech(OPENAI_API_KEY, "tts-1", text, TTS_VOICE, "mp3", "1");

  isSpeaking = true;
  Serial.println("TTS playback started successfully");
}

void printStatistics() {
  Serial.println("=== SYSTEM STATISTICS ===");
  Serial.printf("ESP-NOW Messages Received: %d\n", messagesReceived);
  Serial.printf("Messages Processed: %d\n", messagesProcessed);
  Serial.printf("Messages in Queue: %d/%d\n", messageQueue.count, MAX_MESSAGES_QUEUE);
  Serial.printf("AI Processing: %s\n", aiProcessing ? "Yes" : "No");
  Serial.printf("Currently Speaking: %s\n", isSpeaking ? "Yes" : "No");
  Serial.printf("Pending TTS: %s\n", pendingTTS ? "Yes" : "No");
  if (lastMessageTime > 0) {
    Serial.printf("Last Message: %lu ms ago\n", millis() - lastMessageTime);
  }
  Serial.printf("WiFi Channel: %d\n", WIFI_CHANNEL);
  Serial.printf("WiFi Status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Audio Volume: %d/21\n", audio_player.getVolume());
  Serial.println("========================\n");
}

void showInitializationScreen() {
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(20, 50);
  gfx->print("DEX");
  gfx->setCursor(10, 80);
  gfx->print("Restaurant");

  gfx->setTextSize(1);
  gfx->setTextColor(CYAN);
  gfx->setCursor(10, 120);
  gfx->print("Initializing systems...");

  gfx->setCursor(10, 140);
  gfx->print("- Display: OK");
  gfx->setCursor(10, 155);
  gfx->print("- WiFi: Connecting...");
  gfx->setCursor(10, 170);
  gfx->print("- Audio: Starting...");
  gfx->setCursor(10, 185);
  gfx->print("- ESP-NOW: Standby");
  gfx->setCursor(10, 200);
  gfx->print("- AI: Ready");

  delay(3000);
}

bool validateRequiredFiles() {
  const char* requiredFiles[] = {
    "/home_screen.jpeg",
    "/menu_categories.jpeg",
    "/burgers_menu.jpeg",
    "/sides_menu.jpeg",
    "/drinks_menu.jpeg",
    "/desserts_menu.jpeg"
  };

  int fileCount = sizeof(requiredFiles) / sizeof(requiredFiles[0]);

  for (int i = 0; i < fileCount; i++) {
    if (!validateJpegFile(requiredFiles[i])) {
      Serial.printf("Missing or invalid file: %s\n", requiredFiles[i]);
      return false;
    }
  }

  Serial.println("All required files validated successfully");
  return true;
}

// 7. ADD HELPER FUNCTION TO RESTORE SCREENS
void restoreCurrentCategoryScreen() {
  switch (currentScreen) {
    case SCREEN_BURGERS: displayCategoryScreen("/burgers_menu.jpeg", "BURGERS"); break;
    case SCREEN_SIDES: displayCategoryScreen("/sides_menu.jpeg", "SIDES"); break;
    case SCREEN_DRINKS: displayCategoryScreen("/drinks_menu.jpeg", "DRINKS"); break;
    case SCREEN_DESSERTS: displayCategoryScreen("/desserts_menu.jpeg", "DESSERTS"); break;
    case SCREEN_MENU: displayMenuScreen(); break;
    case SCREEN_HOME: displayHomeScreen(); break;
  }
}

// === UPDATED DISMISS FUNCTION ===
void dismissAIResponse() {
  displayingAIResponse = false;
  currentAIResponse = "";
  currentUserQuestion = "";  // Clear the stored question

  // Stop TTS if still speaking
  if (isSpeaking) {
    audio_player.connecttohost("");  // Stop current audio
    isSpeaking = false;
    Serial.println("TTS stopped due to user interaction");
  }

  // Restore current category screen
  restoreCurrentCategoryScreen();

  Serial.println("AI response dismissed");
}

void handleTouch(int x, int y) {
  // If displaying AI response, any touch should dismiss it
  if (displayingAIResponse) {
    Serial.println("Dismissing AI response due to touch");
    dismissAIResponse();
    return;
  }

  switch (currentScreen) {
    case SCREEN_HOME:
      Serial.println("Going to menu screen");
      playTextToSpeech("Welcome to our menu! Please select a category to explore our delicious options.");
      displayMenuScreen();
      currentScreen = SCREEN_MENU;
      break;

    case SCREEN_MENU:
      for (int i = 0; i < 4; i++) {
        if (isInRegion(x, y, menuRegions[i])) {
          Serial.printf("%s selected\n", menuRegions[i].name);

          if (strcmp(menuRegions[i].name, "BURGERS") == 0) {
            playTextToSpeech("You've selected our burger menu! Here are our delicious burger options. Touch any item to learn more about it.");
            displayCategoryScreen("/burgers_menu.jpeg", "BURGERS");
            currentScreen = SCREEN_BURGERS;
          } else if (strcmp(menuRegions[i].name, "SIDES") == 0) {
            playTextToSpeech("You've selected our sides menu! These tasty sides are perfect complements to any meal.");
            displayCategoryScreen("/sides_menu.jpeg", "SIDES");
            currentScreen = SCREEN_SIDES;
          } else if (strcmp(menuRegions[i].name, "DRINKS") == 0) {
            playTextToSpeech("You've selected our drinks menu! Refresh yourself with our amazing beverage selection.");
            displayCategoryScreen("/drinks_menu.jpeg", "DRINKS");
            currentScreen = SCREEN_DRINKS;
          } else if (strcmp(menuRegions[i].name, "DESSERTS") == 0) {
            playTextToSpeech("You've selected our desserts menu! End your meal with something sweet and delicious.");
            displayCategoryScreen("/desserts_menu.jpeg", "DESSERTS");
            currentScreen = SCREEN_DESSERTS;
          }
          return;
        }
      }
      break;

    case SCREEN_BURGERS:
    case SCREEN_SIDES:
    case SCREEN_DRINKS:
    case SCREEN_DESSERTS:
      for (int i = 0; i < 3; i++) {
        if (isInRegion(x, y, categoryRegions[i])) {
          Serial.printf("%s touched\n", categoryRegions[i].name);

          if (strcmp(categoryRegions[i].name, "BACK") == 0) {
            Serial.println("Going back to menu");
            // Stop current TTS if speaking
            if (isSpeaking) {
              audio_player.connecttohost("");  // Stop current audio
              isSpeaking = false;
            }
            playTextToSpeech("Going back to the main menu.");
            displayMenuScreen();
            currentScreen = SCREEN_MENU;
          } else if (strcmp(categoryRegions[i].name, "ITEM1") == 0) {
            Serial.println("Item 1 selected - Processing with AI");
            handleItemSelection(0);
          } else if (strcmp(categoryRegions[i].name, "ITEM2") == 0) {
            Serial.println("Item 2 selected - Processing with AI");
            handleItemSelection(1);
          }
          return;
        }
      }
      break;
  }
}

void startSynchronizedAIResponse() {
  // Show AI response on screen
  showAIResponse(currentAIResponse);

  // Start TTS immediately (synchronized)
  playTextToSpeech(currentAIResponse);

  // Set display state
  displayingAIResponse = true;
  aiResponseStartTime = millis();

  Serial.println("Started synchronized AI response display + TTS");
}


// === KEEP ORIGINAL showAIResponse FOR BACKWARD COMPATIBILITY ===
void showAIResponse(String response) {
  // If we have a stored user question, use the enhanced version
  if (currentUserQuestion.length() > 0) {
    showAIResponseWithQuestion(currentUserQuestion, response);
  } else {
    // Original function for cases without stored question
    gfx->fillScreen(BLACK);
    gfx->drawRect(5, 5, SCREEN_WIDTH - 10, SCREEN_HEIGHT - 10, CYAN);

    // Header with speaking indicator
    gfx->setTextColor(CYAN);
    gfx->setTextSize(2);
    gfx->setCursor(15, 15);
    gfx->print("DEX Says:");

    // Speaking indicator
    gfx->setTextColor(GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(180, 20);
    gfx->print("🔊");

    // Response text
    gfx->setTextColor(WHITE);
    gfx->setTextSize(1);

    // Word wrap the response (same as before)
    int yPos = 45;
    int xPos = 15;
    String words[100];
    int wordCount = 0;

    // Split response into words
    int lastSpace = 0;
    for (int i = 0; i <= response.length(); i++) {
      if (response.charAt(i) == ' ' || i == response.length()) {
        if (i > lastSpace) {
          words[wordCount] = response.substring(lastSpace, i);
          wordCount++;
          if (wordCount >= 100) break;
        }
        lastSpace = i + 1;
      }
    }

    // Display words with wrapping
    String currentLine = "";
    for (int i = 0; i < wordCount; i++) {
      String testLine = currentLine + words[i] + " ";

      if (testLine.length() * 6 > (SCREEN_WIDTH - 30)) {
        gfx->setCursor(xPos, yPos);
        gfx->print(currentLine);
        yPos += 12;
        currentLine = words[i] + " ";

        if (yPos > (SCREEN_HEIGHT - 50)) break;
      } else {
        currentLine = testLine;
      }
    }

    // Print remaining line
    if (currentLine.length() > 0 && yPos < (SCREEN_HEIGHT - 50)) {
      gfx->setCursor(xPos, yPos);
      gfx->print(currentLine);
    }

    // Enhanced footer with instructions
    gfx->setTextColor(YELLOW);
    gfx->setTextSize(1);
    gfx->setCursor(15, SCREEN_HEIGHT - 35);
    gfx->print("Touch anywhere to continue");
    gfx->setCursor(15, SCREEN_HEIGHT - 20);
    gfx->setTextColor(ORANGE);
    gfx->print("or wait for auto-dismiss");
  }
}

// === UPDATED ITEM SELECTION HANDLER ===
void handleItemSelection(int itemIndex) {
  if (!wifiConnected) {
    showAIStatus("WiFi not connected!", RED, 2000);
    playTextToSpeech("Sorry, I need an internet connection to tell you about this item.");
    return;
  }

  int promptIndex = -1;
  String categoryName = "";

  // Calculate prompt index based on current screen and item
  switch (currentScreen) {
    case SCREEN_BURGERS:
      promptIndex = itemIndex;
      categoryName = "BURGER";
      break;
    case SCREEN_SIDES:
      promptIndex = 2 + itemIndex;
      categoryName = "SIDE";
      break;
    case SCREEN_DRINKS:
      promptIndex = 4 + itemIndex;
      categoryName = "DRINK";
      break;
    case SCREEN_DESSERTS:
      promptIndex = 6 + itemIndex;
      categoryName = "DESSERT";
      break;
    default:
      Serial.println("Invalid screen for item selection");
      return;
  }

  if (promptIndex >= 0 && promptIndex < 8) {
    Serial.printf("Processing %s %d with DEX\n", categoryName.c_str(), itemIndex + 1);

    // STORE THE USER'S ACTION AS A QUESTION
    currentUserQuestion = "Tell me about " + categoryName + " item " + String(itemIndex + 1);

    // Show AI processing status
    showAIStatus("DEX is thinking...", YELLOW, 0);

    // CRITICAL FIX: Properly manage aiProcessing flag
    aiProcessing = true;

    // Send prompt to AI and get response
    String aiResponse = sendToOpenAI(menuPrompts[promptIndex]);

    // CRITICAL FIX: Reset flag after AI call
    aiProcessing = false;

    if (aiResponse != "") {
      Serial.println("AI Response received:");
      Serial.println(aiResponse);

      // Clean the response for better TTS
      aiResponse = cleanResponseForTTS(aiResponse);

      // Store the response and start synchronized display + TTS WITH QUESTION
      currentAIResponse = aiResponse;
      startSynchronizedAIResponseWithQuestion();

    } else {
      showAIStatus("AI connection failed", RED, 3000);
      playTextToSpeech("Sorry, I'm having trouble connecting to get information about this item.");
      // Restore screen after error
      delay(3000);
      restoreCurrentCategoryScreen();
    }
  } else {
    Serial.println("Invalid prompt index calculated");
  }
}

// === NEW FUNCTION FOR SYNCHRONIZED RESPONSE WITH QUESTION ===
void startSynchronizedAIResponseWithQuestion() {
  // Show AI response on screen with question
  showAIResponseWithQuestion(currentUserQuestion, currentAIResponse);

  // Start TTS immediately (synchronized)
  playTextToSpeech(currentAIResponse);

  // Set display state
  displayingAIResponse = true;
  aiResponseStartTime = millis();

  Serial.println("Started synchronized AI response display + TTS with user question");
}

// === FIXED SEND TO OPENAI ===
String sendToOpenAI(const char* prompt) {
  if (!wifiConnected) {
    return "";
  }

  // REMOVED: aiProcessing flag management (handled in calling function)
  String response = Open_AI(String(prompt), OPENAI_API_KEY, false, "");

  return response;
}


void showAIStatus(String message, uint16_t color, int displayTime) {
  // Save current screen area that will be overwritten
  gfx->fillRect(0, 0, SCREEN_WIDTH, 60, BLACK);
  gfx->drawRect(0, 0, SCREEN_WIDTH, 60, color);

  gfx->setTextColor(color);
  gfx->setTextSize(1);
  gfx->setCursor(10, 10);
  gfx->print("DEX:");

  gfx->setTextColor(WHITE);
  gfx->setCursor(10, 25);
  gfx->print(message);

  gfx->setTextColor(CYAN);
  gfx->setTextSize(1);
  gfx->setCursor(10, 40);
  gfx->print("Processing your request...");

  if (displayTime > 0) {
    delay(displayTime);
  }
}

// === NEW ENHANCED AI RESPONSE DISPLAY FUNCTION ===
void showAIResponseWithQuestion(String userQuestion, String aiResponse) {
  gfx->fillScreen(BLACK);
  gfx->drawRect(5, 5, SCREEN_WIDTH - 10, SCREEN_HEIGHT - 10, CYAN);

  // Header with speaking indicator
  gfx->setTextColor(CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(15, 15);
  gfx->print("DEX Says:");

  // Speaking indicator
  gfx->setTextColor(GREEN);
  gfx->setTextSize(1);
  gfx->setCursor(180, 20);
  gfx->print("🔊");

  // === USER QUESTION SECTION ===
  gfx->setTextColor(YELLOW);
  gfx->setTextSize(1);
  gfx->setCursor(15, 40);
  gfx->print("You asked:");

  // Display user question with word wrap
  gfx->setTextColor(WHITE);
  int yPos = 55;
  int xPos = 15;

  // Word wrap the user question
  String questionLines[5];  // Max 5 lines for question
  int questionLineCount = 0;

  // Split question into words and wrap
  String words[50];
  int wordCount = 0;
  int lastSpace = 0;

  for (int i = 0; i <= userQuestion.length(); i++) {
    if (userQuestion.charAt(i) == ' ' || i == userQuestion.length()) {
      if (i > lastSpace) {
        words[wordCount] = userQuestion.substring(lastSpace, i);
        wordCount++;
        if (wordCount >= 50) break;
      }
      lastSpace = i + 1;
    }
  }

  // Create wrapped lines for question
  String currentLine = "";
  for (int i = 0; i < wordCount && questionLineCount < 5; i++) {
    String testLine = currentLine + words[i] + " ";

    if (testLine.length() * 6 > (SCREEN_WIDTH - 30)) {
      questionLines[questionLineCount] = currentLine;
      questionLineCount++;
      currentLine = words[i] + " ";
    } else {
      currentLine = testLine;
    }
  }

  // Add remaining line
  if (currentLine.length() > 0 && questionLineCount < 5) {
    questionLines[questionLineCount] = currentLine;
    questionLineCount++;
  }

  // Display question lines
  for (int i = 0; i < questionLineCount; i++) {
    gfx->setCursor(xPos, yPos);
    gfx->print(questionLines[i]);
    yPos += 12;
  }

  // === SEPARATOR LINE ===
  yPos += 5;
  gfx->drawLine(15, yPos, SCREEN_WIDTH - 15, yPos, DARKGREY);
  yPos += 10;

  // === AI RESPONSE SECTION ===
  gfx->setTextColor(CYAN);
  gfx->setCursor(15, yPos);
  gfx->print("DEX answers:");
  yPos += 15;

  // Response text
  gfx->setTextColor(WHITE);

  // Word wrap the AI response
  String responseWords[100];
  int responseWordCount = 0;
  lastSpace = 0;

  // Split response into words
  for (int i = 0; i <= aiResponse.length(); i++) {
    if (aiResponse.charAt(i) == ' ' || i == aiResponse.length()) {
      if (i > lastSpace) {
        responseWords[responseWordCount] = aiResponse.substring(lastSpace, i);
        responseWordCount++;
        if (responseWordCount >= 100) break;
      }
      lastSpace = i + 1;
    }
  }

  // Display response words with wrapping
  currentLine = "";
  for (int i = 0; i < responseWordCount; i++) {
    String testLine = currentLine + responseWords[i] + " ";

    if (testLine.length() * 6 > (SCREEN_WIDTH - 30)) {
      gfx->setCursor(xPos, yPos);
      gfx->print(currentLine);
      yPos += 12;
      currentLine = responseWords[i] + " ";

      if (yPos > (SCREEN_HEIGHT - 50)) break;
    } else {
      currentLine = testLine;
    }
  }

  // Print remaining line
  if (currentLine.length() > 0 && yPos < (SCREEN_HEIGHT - 50)) {
    gfx->setCursor(xPos, yPos);
    gfx->print(currentLine);
  }

  // === ENHANCED FOOTER ===
  gfx->setTextColor(YELLOW);
  gfx->setTextSize(1);
  gfx->setCursor(15, SCREEN_HEIGHT - 35);
  gfx->print("Touch anywhere to continue");
  gfx->setCursor(15, SCREEN_HEIGHT - 20);
  gfx->setTextColor(ORANGE);
  gfx->print("or wait for auto-dismiss");
}

// 10. OPTIONAL: ADD FUNCTION TO HANDLE INTERRUPTING TTS
void interruptCurrentTTS(String newMessage) {
  if (isSpeaking) {
    audio_player.connecttohost("");  // Stop current audio
    isSpeaking = false;
    Serial.println("Current TTS interrupted for new message");
  }

  // Clear any pending TTS
  pendingTTS = false;
  pendingMessage = "";

  // Start new TTS immediately
  playTextToSpeech(newMessage);
}

bool isInRegion(int x, int y, TouchRegion region) {
  return (x >= region.x1 && x <= region.x2 && y >= region.y1 && y <= region.y2);
}

void displayHomeScreen() {
  Serial.println("=== Displaying Home Screen ===");
  gfx->fillScreen(BLACK);

  if (!loadAndDisplayJpeg("/home_screen.jpeg", 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, false)) {
    Serial.println("Home screen image load failed, showing placeholder");
    drawImagePlaceholder(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, "HOME SCREEN");

    // Add AI status indicator
    if (wifiConnected) {
      gfx->setTextColor(GREEN);
      gfx->setTextSize(1);
      gfx->setCursor(10, 300);
      gfx->print("DEX : Ready");
    } else {
      gfx->setTextColor(RED);
      gfx->setTextSize(1);
      gfx->setCursor(10, 300);
      gfx->print("DEX : Offline");
    }
  }
}

void displayMenuScreen() {
  Serial.println("=== Displaying Menu Screen ===");
  gfx->fillScreen(BLACK);

  if (!loadAndDisplayJpeg("/menu_categories.jpeg", 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, false)) {
    Serial.println("Menu categories image load failed, showing placeholder");
    drawImagePlaceholder(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, "MENU CATEGORIES");
  }
}

void displayCategoryScreen(const char* imageFile, const char* categoryName) {
  Serial.printf("=== Displaying Category Screen: %s ===\n", categoryName);
  gfx->fillScreen(BLACK);

  if (!loadAndDisplayJpeg(imageFile, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, false)) {
    Serial.printf("Category image load failed: %s\n", imageFile);
    drawImagePlaceholder(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, categoryName);
  }
}

void drawImagePlaceholder(int x, int y, int w, int h, const char* label) {
  gfx->fillRect(x, y, w, h, DARKGREY);
  gfx->drawRect(x, y, w, h, WHITE);

  gfx->setTextColor(RED);
  gfx->setTextSize(2);
  int labelWidth = strlen(label) * 6 * 2;
  int labelX = x + (w - labelWidth) / 2;
  int labelY = y + (h - 16) / 2;
  gfx->setCursor(labelX, labelY);
  gfx->print(label);
}

void listSDFiles() {
  File root = SD_MMC.open("/");
  if (root) {
    Serial.println("Files in SD card root:");
    File file = root.openNextFile();
    while (file) {
      Serial.printf("  %s (%d bytes)\n", file.name(), file.size());
      file = root.openNextFile();
    }
    root.close();
  }
}

bool validateJpegFile(const char* filename) {
  if (!SD_MMC.exists(filename)) {
    Serial.printf("File not found: %s\n", filename);
    return false;
  }

  File file = SD_MMC.open(filename, FILE_READ);
  if (!file) {
    Serial.printf("Cannot open file: %s\n", filename);
    return false;
  }

  size_t fileSize = file.size();
  Serial.printf("Found %s - Size: %d bytes\n", filename, fileSize);

  if (fileSize < 100) {
    Serial.printf("File too small: %s\n", filename);
    file.close();
    return false;
  }

  uint8_t header[4];
  file.read(header, 4);

  if (header[0] != 0xFF || header[1] != 0xD8) {
    Serial.printf("Invalid JPEG header in %s: 0x%02X 0x%02X\n", filename, header[0], header[1]);
    file.close();
    return false;
  }

  file.close();
  Serial.printf("Valid JPEG file: %s\n", filename);
  return true;
}

bool loadAndDisplayJpeg(const char* filename, int xpos, int ypos, int maxWidth, int maxHeight, bool maintainAspect) {
  Serial.printf("\n--- Loading %s ---\n", filename);
  Serial.printf("Position: (%d, %d), Size: %dx%d\n", xpos, ypos, maxWidth, maxHeight);

  if (!SD_MMC.exists(filename)) {
    Serial.printf("ERROR: File not found: %s\n", filename);
    return false;
  }

  JpegDec.abort();
  delay(10);

  bool decodeSuccess = false;

  File jpegFile = SD_MMC.open(filename, FILE_READ);
  if (jpegFile && jpegFile.size() < 200000) {
    size_t fileSize = jpegFile.size();
    Serial.printf("File size: %d bytes, allocating buffer...\n", fileSize);

    uint8_t* buffer = (uint8_t*)malloc(fileSize);
    if (buffer) {
      size_t bytesRead = jpegFile.read(buffer, fileSize);
      jpegFile.close();

      if (bytesRead == fileSize) {
        Serial.printf("Buffer loaded (%d bytes), attempting decode...\n", bytesRead);

        if (JpegDec.decodeArray(buffer, fileSize)) {
          Serial.printf("Buffer decode success: %dx%d\n", JpegDec.width, JpegDec.height);

          if (JpegDec.width > 0 && JpegDec.height > 0 && JpegDec.width < 2000 && JpegDec.height < 2000) {
            decodeSuccess = true;
          } else {
            Serial.printf("Invalid dimensions: %dx%d\n", JpegDec.width, JpegDec.height);
            JpegDec.abort();
          }
        }
      }
      free(buffer);
    } else {
      Serial.println("Failed to allocate buffer");
      jpegFile.close();
    }
  } else {
    if (jpegFile) {
      Serial.printf("File too large (%d bytes) or failed to open\n", jpegFile.size());
      jpegFile.close();
    }
  }

  if (!decodeSuccess || JpegDec.width == 0 || JpegDec.height == 0) {
    Serial.printf("JPEG decode failed! Dimensions: %dx%d\n", JpegDec.width, JpegDec.height);
    return false;
  }

  Serial.printf("JPEG decode successful: %dx%d\n", JpegDec.width, JpegDec.height);

  return renderJpegSimple(xpos, ypos);
}

bool renderJpegSimple(int offsetX, int offsetY) {
  Serial.printf("Starting render at (%d,%d)...\n", offsetX, offsetY);

  uint16_t* pImg;
  uint16_t mcu_w = JpegDec.MCUWidth;
  uint16_t mcu_h = JpegDec.MCUHeight;
  uint32_t max_x = gfx->width();
  uint32_t max_y = gfx->height();

  if (mcu_w == 0 || mcu_h == 0 || mcu_w > 32 || mcu_h > 32) {
    Serial.printf("Invalid MCU dimensions: %dx%d\n", mcu_w, mcu_h);
    return false;
  }

  uint32_t win_w = JpegDec.width;
  uint32_t win_h = JpegDec.height;

  Serial.printf("Render window: %dx%d, MCU: %dx%d\n", win_w, win_h, mcu_w, mcu_h);

  uint16_t* lineBuffer = (uint16_t*)malloc(mcu_w * 2);
  if (!lineBuffer) {
    Serial.println("Failed to allocate line buffer");
    return false;
  }

  uint32_t mcu_x = 0;
  uint32_t mcu_y = 0;

  while (JpegDec.read()) {
    pImg = JpegDec.pImage;

    int img_x = offsetX + mcu_x;
    int img_y = offsetY + mcu_y;

    if (img_x >= max_x || img_y >= max_y) {
      break;
    }

    uint32_t render_w = min(mcu_w, max_x - img_x);
    uint32_t render_h = min(mcu_h, max_y - img_y);

    if (render_w <= 0 || render_h <= 0) {
      mcu_x += mcu_w;
      if (mcu_x >= win_w) {
        mcu_x = 0;
        mcu_y += mcu_h;
      }
      continue;
    }

    for (uint32_t y = 0; y < render_h; y++) {
      memcpy(lineBuffer, pImg + (y * mcu_w), render_w * 2);
      gfx->draw16bitRGBBitmap(img_x, img_y + y, lineBuffer, render_w, 1);
    }

    mcu_x += mcu_w;
    if (mcu_x >= win_w) {
      mcu_x = 0;
      mcu_y += mcu_h;

      if (mcu_y >= win_h) {
        break;
      }
    }
  }

  free(lineBuffer);
  Serial.println("JPEG render completed successfully");
  return true;
}

void showError(const char* errorMsg) {
  Serial.printf("ERROR: %s\n", errorMsg);

  gfx->fillScreen(BLACK);
  gfx->setTextColor(RED);
  gfx->setTextSize(2);
  gfx->setCursor(10, 50);
  gfx->print("ERROR:");

  gfx->setTextColor(WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(10, 80);
  gfx->print(errorMsg);

  gfx->setCursor(10, 100);
  gfx->print("Check SD card and files");
}

// --- OpenAI Integration Functions ---
String Open_AI(String UserRequest, const char* LLM_API_key, bool flg_WebSearch, String UserCity) {
  uint32_t t_start = millis();

  static bool flg_INITIALIZED_ALREADY = false;
  if (!flg_INITIALIZED_ALREADY) {
    MESSAGES.reserve(MESSAGES_SIZE);
    flg_INITIALIZED_ALREADY = true;
  }

  if (UserRequest == "") {
    return ("");
  }

  if (UserRequest == "#") {
    Serial.println("\nMESSAGES (CHAT PROMPT LOG):");
    Serial.println(MESSAGES);
    return ("");
  }

  String OpenAI_Response = "";
  String Feedback = "";

  String LLM_server = "api.openai.com";
  String LLM_entrypoint = "/v1/chat/completions";
  String LLM_model = "gpt-4o-mini";
  if (flg_WebSearch) {
    LLM_model = "gpt-4o-mini-search-preview";
  }

  // Additional cleaning (needed for url syntax):
  UserRequest.replace("\"", "\\\"");

  WiFiClientSecure client_tcp;
  client_tcp.setInsecure();

  // Creating the Payload:
  String request_Prefix, request_Content, request_Postfix, request_LEN;

  request_Prefix = "{#model#:#" + LLM_model + "#, #messages#:[";
  request_Content = ",\n{#role#: #user#, #content#: #" + UserRequest + "#}],\n";

  if (!flg_WebSearch) {
    request_Postfix = "#temperature#:0.7, #max_tokens#:150, #presence_penalty#:0.6, #top_p#:1.0}";
  } else {
    request_Postfix = "#response_format#: {#type#: #text#}, ";
    request_Postfix += "#web_search_options#: {#search_context_size#: #low#, ";
    request_Postfix += "#user_location#: {#type#: #approximate#, #approximate#: ";
    request_Postfix += "{#country#: ##, #city#: #" + UserCity + "#}}}, ";
    request_Postfix += "#store#: false}";
  }

  request_Prefix.replace("#", "\"");
  request_Content.replace("#", "\"");
  request_Postfix.replace("#", "\"");
  request_LEN = (String)(MESSAGES.length() + request_Prefix.length() + request_Content.length() + request_Postfix.length());

  uint32_t t_startRequest = millis();

  // Now sending the request:
  if (client_tcp.connect(LLM_server.c_str(), 443)) {
    client_tcp.println("POST " + LLM_entrypoint + " HTTP/1.1");
    client_tcp.println("Connection: close");
    client_tcp.println("Host: " + LLM_server);
    client_tcp.println("Authorization: Bearer " + (String)LLM_API_key);
    client_tcp.println("Content-Type: application/json; charset=utf-8");
    client_tcp.println("Content-Length: " + request_LEN);
    client_tcp.println();
    client_tcp.print(request_Prefix);
    client_tcp.print(MESSAGES);
    client_tcp.println(request_Content + request_Postfix);

    // Now reading the complete tcp message body:
    t_startRequest = millis();
    OpenAI_Response = "";

    while (millis() < (t_startRequest + (TIMEOUT_OPEN_AI * 1000)) && OpenAI_Response == "") {
      Serial.print(".");
      delay(250);
      while (client_tcp.available()) {
        char c = client_tcp.read();
        OpenAI_Response += String(c);
      }
    }
    client_tcp.stop();
  } else {
    Serial.println("* ERROR: client_tcp.connect(\"api.openai.com\", 443) failed !");
    return ("");
  }

  uint32_t t_response = millis();

  // Now extracting clean message for return value 'Feedback':
  int pos_start, pos_end;
  bool found = false;
  pos_start = OpenAI_Response.indexOf("\"content\":");
  if (pos_start > 0) {
    pos_start += 12;
    pos_end = pos_start + 1;
    while (!found) {
      found = true;
      pos_end = OpenAI_Response.indexOf("\"", pos_end);
      if (pos_end > 0) {
        if (OpenAI_Response.substring(pos_end - 1, pos_end) == "\\") {
          found = false;
          pos_end++;
        }
      }
    }
  }
  if (pos_start > 0 && (pos_end > pos_start)) {
    Feedback = OpenAI_Response.substring(pos_start, pos_end);
    Feedback.trim();
  }

  // APPEND current I/O chat (UserRequest & Feedback) at end of var MESSAGES
  if (Feedback != "") {
    String NewMessagePair = ",\n";
    if (MESSAGES == "") {
      NewMessagePair = "";
    }
    NewMessagePair += "{\"role\": \"user\", \"content\": \"" + UserRequest + "\"},\n";
    NewMessagePair += "{\"role\": \"assistant\", \"content\": \"" + Feedback + "\"}";

    MESSAGES += NewMessagePair;
  }

  // Finally we clean Feedback, print DEBUG Latency info and return 'Feedback' String
  if (Feedback != "") {
    Feedback.replace("\\n", "\n");
    Feedback.replace("\\\"", "\"");
    Feedback.trim();
  }

  DebugPrintln("\n--------------------------------------------");
  DebugPrintln("Open AI LLM - model: [" + LLM_model + "]");
  DebugPrintln("-> Latency Open AI Server (Re)CONNECT:  " + (String)((float)((t_startRequest - t_start)) / 1000));
  DebugPrintln("-> Latency Open AI LLM Response:        " + (String)((float)((t_response - t_startRequest)) / 1000));
  DebugPrintln("=> TOTAL Duration [sec]: .............. " + (String)((float)((t_response - t_start)) / 1000));
  DebugPrintln("--------------------------------------------");
  DebugPrint("Open AI LLM>");

  return (Feedback);
}

// Additional utility functions for ESP-NOW and audio control
void processKeywords(String transcription) {
  String lowerTranscription = transcription;
  lowerTranscription.toLowerCase();

  Serial.println("\nKeyword Detection:");

  // Greetings
  if (lowerTranscription.indexOf("hello") >= 0 || lowerTranscription.indexOf("hi") >= 0 || lowerTranscription.indexOf("hey") >= 0) {
    Serial.println("Greeting detected!");
  }

  // Menu related
  if (lowerTranscription.indexOf("menu") >= 0 || lowerTranscription.indexOf("food") >= 0 || lowerTranscription.indexOf("burger") >= 0) {
    Serial.println("Menu inquiry detected!");
  }

  // Help requests
  if (lowerTranscription.indexOf("help") >= 0 || lowerTranscription.indexOf("assist") >= 0) {
    Serial.println("Help request detected!");
  }

  // Control commands
  if (lowerTranscription.indexOf("stop") >= 0) {
    Serial.println("Stop command detected!");
    if (isSpeaking) {
      audio_player.connecttohost("");  // Stop current audio
      isSpeaking = false;
      aiProcessing = false;
      Serial.println("TTS stopped by command");
    }
  }

  // Volume control
  if (lowerTranscription.indexOf("louder") >= 0 || lowerTranscription.indexOf("volume up") >= 0) {
    Serial.println("Volume up detected!");
    int currentVol = audio_player.getVolume();
    if (currentVol < 21) {
      audio_player.setVolume(currentVol + 2);
      Serial.printf("Volume increased to: %d\n", audio_player.getVolume());
    }
  }

  if (lowerTranscription.indexOf("quieter") >= 0 || lowerTranscription.indexOf("volume down") >= 0) {
    Serial.println("Volume down detected!");
    int currentVol = audio_player.getVolume();
    if (currentVol > 0) {
      audio_player.setVolume(currentVol - 2);
      Serial.printf("Volume decreased to: %d\n", audio_player.getVolume());
    }
  }

  // Questions
  if (lowerTranscription.indexOf("what") >= 0 && lowerTranscription.indexOf("?") >= 0) {
    Serial.println("Question detected!");
  }

  Serial.println("=== END KEYWORD PROCESSING ===\n");
}

// Utility functions
String getLastMessage() {
  return String(receivedMessage);
}

bool isNewMessageAvailable() {
  return newMessageReceived;
}

bool isCurrentlySpeaking() {
  return isSpeaking;
}

bool isProcessingAI() {
  return aiProcessing;
}

void getStatistics(int& received, int& processed, unsigned long& lastTime) {
  received = messagesReceived;
  processed = messagesProcessed;
  lastTime = lastMessageTime;
}

void resetStatistics() {
  messagesReceived = 0;
  messagesProcessed = 0;
  lastMessageTime = 0;
  Serial.println("Statistics reset");
}

void setTTSVolume(int volume) {
  if (volume >= 0 && volume <= 21) {
    audio_player.setVolume(volume);
    Serial.printf("Volume set to: %d/21\n", volume);
  } else {
    Serial.println("Volume must be between 0 and 21");
  }
}

// Optional callback for audio events
void audio_info(const char* info) {
  Serial.println("Audio info: " + String(info));
}