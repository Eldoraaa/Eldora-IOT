# DoraBot

ESP32-S3 firmware for Eldora DoraBot, the home companion device that handles Wi-Fi provisioning, pairing, voice capture, voice playback, LCD status, heartbeat, and backend command polling.

## Hardware
- ESP32-S3
- MAX98357 I2S amplifier
- Speaker
- INMP441 microphone
- 3.5 inch LCD
- Step-up + charger module
- Li-ion battery

## Firmware features
- Local setup AP for Wi-Fi provisioning
- Mobile pairing token flow
- Backend heartbeat reporting
- Backend command polling
- DoraBot voice capture upload
- DoraBot voice playback from backend audio URLs
- LCD status and caregiver messages
- Wi-Fi scan/apply flow
- Battery and signal telemetry

## Configure
Update constants in `Eldora.ino` before flashing:
- backend URL
- device key
- provisioning secret
- firmware version
- hardware pins if wiring changes

## Build / flash
Open `Eldora.ino` in Arduino IDE, select the ESP32-S3 target board, install required libraries, then upload.

## Libraries
- WiFi / HTTPClient / WebServer / ESPmDNS
- ArduinoJson
- Preferences
- LovyanGFX
- ESP8266Audio-compatible audio classes
- ESP32 I2S driver
