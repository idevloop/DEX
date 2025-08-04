# DEX – An Interactive AI Kiosk for Restaurants

DEX isn't just a kiosk. It's an intelligent, multilingual, voice-interactive assistant that helps users browse, ask, and decide what to eat — just like talking to a helpful human at the counter.

##  Features

-  **Voice Interaction** – Ask questions about food items, ingredients, or suggestions.
-  **Multilingual Support** – Break language barriers with real-time voice-to-text and text-to-speech.
-  **Touch Navigation** – Tap on menu items and categories to hear audio descriptions instantly.
-  **AI Assistant Integration** – Powered by OpenAI for natural conversations and smart suggestions.
-  **ESP-NOW Communication** – Dual ESP32 devices for fast, low-latency communication between mic and display.
-  **Offline Recording** – Record voice via button-hold and send to ElevenLabs for transcription.

##  How It Works

DEX is built on two ESP32-S3-based boards:

- **XIAO ESP32S3 Sense**: Captures user voice, converts it into text using ElevenLabs API, then sends it over ESP-NOW.
- **MaTouch ESP32S3 Display**: Receives the text, processes it via OpenAI's Chat API, then responds with spoken replies using OpenAI TTS and an I2S speaker.

Users can interact in two ways:
- **Voice Mode**: Press and hold the mic button, ask a question (e.g., "Is the burger vegetarian?"), and receive a voice reply.
- **Touch Mode**: Browse the image-based menu and tap an item — DEX reads out the item details instantly.

##  Hardware Used

- [XIAO ESP32S3 Sense]– With built-in microphone and ESP32S3 SoC
- [MaTouch ESP32S3 Display] – With touch display + SD card for menu images
- MAX98357A I2S amplifier + speaker
