# IoT Eldora

Arduino firmware for Eldora devices.

## DoraBot hardware
- ESP32-S3
- MAX98357 I2S amplifier
- Speaker
- INMP441 microphone
- 3.5 inch LCD
- Step-up + charger module
- Li-ion battery

## Fall detection hardware
- ESP32-C3
- MPU6050
- MAX30102
- Step-up + charger module
- Li-ion battery

## Firmware features
- WiFi provisioning via local setup AP
- Local pairing token for mobile pairing
- Backend heartbeat and command polling
- DoraBot voice capture/playback
- LCD status display
- WiFi command apply flow

## Configure
Update the constants in `Eldora.ino` before flashing:
- backend URL
- device key
- provisioning secret
- firmware version
- hardware pins if wiring changes

## Build/flash
Open `Eldora.ino` in Arduino IDE, select the target ESP32 board, install required libraries, then upload.

## Libraries
- WiFi / HTTPClient / WebServer / ESPmDNS
- ArduinoJson
- Preferences
- LovyanGFX
- ESP8266Audio-compatible audio classes
- ESP32 I2S driver
