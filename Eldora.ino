#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_system.h>
#include <LovyanGFX.hpp>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourcePROGMEM.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include "images_data.h"

#ifndef ELDORA_FRAME_WIDTH
#define ELDORA_FRAME_WIDTH 16
#endif

#ifndef ELDORA_FRAME_HEIGHT
#define ELDORA_FRAME_HEIGHT 16
#endif

#define ELDORA_PRODUCT_NAME "ELDORA_CARE"
#define ELDORA_BACKEND_URL "https://eldora-backend-production.up.railway.app"
#define ELDORA_DEVICE_KEY "ELDORA-HUB-001"
#define ELDORA_PROVISIONING_SECRET "bZdarnuQqe6EpTftziZaCeNgGR4phm0984V_nF1w0tk"
#define ELDORA_FIRMWARE_VERSION "0.4.1"

struct EldoraConfig {
  const char* productName;
  const char* backendUrl;
  const char* deviceKey;
  const char* provisioningSecret;
  const char* firmwareVersion;
};

const EldoraConfig CONFIG = {
  ELDORA_PRODUCT_NAME,
  ELDORA_BACKEND_URL,
  ELDORA_DEVICE_KEY,
  ELDORA_PROVISIONING_SECRET,
  ELDORA_FIRMWARE_VERSION,
};

// TFT pins.
#define TFT_SCK 12
#define TFT_MOSI 11
#define TFT_CS 10
#define TFT_DC 9
#define TFT_RST 8
#define TFT_BL 14

// Audio pins.
#define MIC_SCK 41
#define MIC_WS 42
#define MIC_SD 2
#define AMP_BCLK 16
#define AMP_LRC 15
#define AMP_DIN 17
#define VOICE_BUTTON_PIN 1
#define SAMPLE_RATE 16000
#define REC_SECONDS 8
#define SILENCE_THRESHOLD 700
#define VOICE_START_SAMPLES (SAMPLE_RATE / 20)
#define VOICE_END_SILENCE_SAMPLES SAMPLE_RATE
#define VOICE_MIN_SPEECH_SAMPLES (SAMPLE_RATE / 4)
#define VOICE_MIN_RECORD_BYTES SAMPLE_RATE
#define VOICE_FEEDBACK_COOLDOWN_MS 2500UL
#define VOICE_NOISE_COOLDOWN_MS 800UL

// Timers.
#define HEARTBEAT_INTERVAL_MS 30000UL
#define COMMAND_POLL_INTERVAL_MS 3000UL
#define DISPLAY_INTERVAL_MS 500UL
#define WIFI_CONNECT_TIMEOUT_MS 20000UL

// Persistent hub settings.
#define PREF_NAMESPACE "eldora"
#define PREF_WIFI_SSID "wifi_ssid"
#define PREF_WIFI_PASS "wifi_pass"
#define PREF_PAIRING_TOKEN "local_pairing_token"

// Set this to an ADC pin after adding a voltage divider circuit.
#define BATTERY_ADC_PIN -1
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200

enum HubState { BOOTING, PROVISIONING, WIFI_CONNECTING, ONLINE, APPLYING_WIFI, LISTENING, THINKING, SPEAKING, ERROR_STATE };
HubState currentState = BOOTING;

class LGFX_ESP32S3 : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX_ESP32S3() {
    auto busConfig = _bus.config();
    busConfig.spi_host = SPI2_HOST;
    busConfig.pin_sclk = TFT_SCK;
    busConfig.pin_mosi = TFT_MOSI;
    busConfig.pin_miso = -1;
    busConfig.pin_dc = TFT_DC;
    busConfig.freq_write = 15000000;
    _bus.config(busConfig);
    _panel.setBus(&_bus);

    auto panelConfig = _panel.config();
    panelConfig.pin_cs = TFT_CS;
    panelConfig.pin_rst = TFT_RST;
    panelConfig.panel_width = 320;
    panelConfig.panel_height = 480;
    panelConfig.rgb_order = true;
    _panel.config(panelConfig);
    setPanel(&_panel);
  }
};

LGFX_ESP32S3 tft;
Preferences preferences;
WebServer provisioningServer(80);

// --- MUTEX UNTUK MENGAMANKAN LAYAR ---
SemaphoreHandle_t tftMutex;

String activeWifiSsid;
String activeWifiPass;
String pendingWifiSsid;
String pendingWifiPass;
String localPairingToken;
uint8_t* audioBuffer = nullptr;
size_t audioBufferSize = SAMPLE_RATE * 2 * REC_SECONDS;
size_t currentAudioSize = 0;
volatile int16_t micVolume = 0;
AudioGeneratorMP3* mp3 = nullptr;
AudioOutputI2S* audioOut = nullptr;
bool provisioningMode = false;
bool shouldApplyProvisionedWifi = false;
bool localServerStarted = false;
bool mdnsStarted = false;
unsigned long voiceCooldownUntil = 0;

String compactDeviceCode() {
  String code = CONFIG.deviceKey;
  code.replace("-", "");
  if (code.length() <= 6) return code;
  return code.substring(code.length() - 6);
}

String provisioningSsid() {
  return String("ELDORA-SETUP-") + compactDeviceCode();
}

String provisioningPassword() {
  String code = compactDeviceCode();
  if (code.length() <= 3) return String("eldora") + code;
  return String("eldora") + code.substring(code.length() - 3);
}

String generatePairingToken() {
  char token[25];
  snprintf(
    token,
    sizeof(token),
    "%08lX%08lX%08lX",
    (unsigned long)esp_random(),
    (unsigned long)esp_random(),
    (unsigned long)esp_random()
  );
  return String(token);
}

bool isTemplatePairingToken(const String& token) {
  String normalized = token;
  normalized.toLowerCase();
  return normalized == "pair_token" ||
         normalized == "pairing_token" ||
         normalized == "template" ||
         normalized == "changeme" ||
         normalized == "change_me" ||
         normalized == "your_token";
}

void loadPairingToken() {
  localPairingToken = preferences.getString(PREF_PAIRING_TOKEN, "");
  if (localPairingToken.length() >= 16 && !isTemplatePairingToken(localPairingToken)) return;

  localPairingToken = generatePairingToken();
  preferences.putString(PREF_PAIRING_TOKEN, localPairingToken);
  Serial.println("[PAIR] Local pairing token generated");
}

const char* stateLabel() {
  switch (currentState) {
    case BOOTING:
      return "Booting";
    case PROVISIONING:
      return "Setup WiFi";
    case WIFI_CONNECTING:
      return "Connecting";
    case ONLINE:
      return "Online";
    case APPLYING_WIFI:
      return "WiFi Setup";
    case LISTENING:
      return "Listening";
    case THINKING:
      return "Thinking";
    case SPEAKING:
      return "Speaking";
    case ERROR_STATE:
      return "Needs Check";
    default:
      return "Unknown";
  }
}

uint16_t stateColor() {
  switch (currentState) {
    case ONLINE:
      return lgfx::color565(80, 190, 135);
    case PROVISIONING:
      return lgfx::color565(123, 167, 212);
    case APPLYING_WIFI:
      return lgfx::color565(90, 150, 230);
    case LISTENING:
      return lgfx::color565(90, 150, 230);
    case THINKING:
      return lgfx::color565(214, 158, 46);
    case SPEAKING:
      return lgfx::color565(80, 190, 135);
    case ERROR_STATE:
      return lgfx::color565(235, 95, 95);
    default:
      return lgfx::color565(255, 138, 122);
  }
}

const uint8_t* currentFrame() {
  if (currentState == SPEAKING || currentState == ONLINE) return img_speak;
  if (currentState == PROVISIONING || currentState == WIFI_CONNECTING || currentState == APPLYING_WIFI || currentState == LISTENING || currentState == THINKING) return img_listen;
  return img_idle;
}

void drawFrameBitmap(const uint8_t* frame, int x, int y, int scale, uint16_t color) {
  for (int row = 0; row < ELDORA_FRAME_HEIGHT; row++) {
    for (int col = 0; col < ELDORA_FRAME_WIDTH; col++) {
      int byteIndex = row * (ELDORA_FRAME_WIDTH / 8) + (col / 8);
      uint8_t byteValue = pgm_read_byte(&frame[byteIndex]);
      bool isOn = byteValue & (0x80 >> (col % 8));
      if (isOn) {
        tft.fillRoundRect(x + col * scale, y + row * scale, scale - 1, scale - 1, 2, color);
      }
    }
  }
}

void renderDisplay() {
  static HubState lastState = BOOTING;
  static unsigned long lastRender = 0;

  if (lastState == currentState && millis() - lastRender < DISPLAY_INTERVAL_MS) {
    return;
  }

  lastState = currentState;
  lastRender = millis();

  // MENGAMANKAN AKSES LAYAR DENGAN MUTEX
  if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
    tft.fillScreen(lgfx::color565(234, 245, 251));
    tft.fillCircle(240, 115, 52, stateColor());
    tft.fillCircle(240, 115, 34, lgfx::color565(16, 24, 39));
    drawFrameBitmap(currentFrame(), 192, 67, 6, TFT_WHITE);

    tft.setTextDatum(middle_center);
    tft.setTextColor(lgfx::color565(31, 42, 55));
    tft.setTextSize(3);
    tft.drawString("ELDORA", 240, 200);

    tft.setTextColor(lgfx::color565(92, 113, 132));
    tft.setTextSize(2);
    tft.drawString(stateLabel(), 240, 242);

    tft.setTextSize(1);
    if (currentState == PROVISIONING) {
      tft.drawString(provisioningSsid(), 240, 278);
      tft.drawString(String("Pass: ") + provisioningPassword(), 240, 302);
      tft.drawString("Open 192.168.4.1", 240, 326);
    } else {
      tft.drawString(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "No WiFi", 240, 280);
      tft.drawString(String("Key: ") + String(CONFIG.deviceKey).substring(0, 8) + "...", 240, 305);
    }
    xSemaphoreGive(tftMutex); // LEPASKAN KUNCI
  }
}

int readBatteryLevel() {
  if (BATTERY_ADC_PIN < 0) return -1;

  int raw = analogRead(BATTERY_ADC_PIN);
  int mv = map(raw, 0, 4095, 0, 5000);
  int percent = map(mv, BATTERY_EMPTY_MV, BATTERY_FULL_MV, 0, 100);
  return constrain(percent, 0, 100);
}

void loadWifiCredentials() {
  activeWifiSsid = preferences.getString(PREF_WIFI_SSID, "");
  activeWifiPass = preferences.getString(PREF_WIFI_PASS, "");

  Serial.printf(
    "[WIFI] Active SSID: %s\n",
    activeWifiSsid.length() > 0 ? activeWifiSsid.c_str() : "(not configured)"
  );
}

bool hasWifiCredentials() {
  return preferences.isKey(PREF_WIFI_SSID) && preferences.getString(PREF_WIFI_SSID, "").length() > 0;
}

void saveWifiCredentials(const String& ssid, const String& password) {
  preferences.putString(PREF_WIFI_SSID, ssid);
  preferences.putString(PREF_WIFI_PASS, password);

  activeWifiSsid = ssid;
  activeWifiPass = password;
  Serial.printf("[WIFI] Saved SSID: %s\n", activeWifiSsid.c_str());
}

void startMdns() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) return;

  if (MDNS.begin("eldora-hub-001")) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    Serial.println("[MDNS] eldora-hub-001.local");
  }
}

void ensureLocalServer();
void showCoreMessage(const String& title, const String& message);

bool canAutoListen() {
  return WiFi.status() == WL_CONNECTED && millis() >= voiceCooldownUntil && currentState == LISTENING;
}

void addDeviceHeaders(HTTPClient& http) {

  http.addHeader("X-Device-Key", CONFIG.deviceKey);
  if (CONFIG.provisioningSecret[0] != '\0') {
    http.addHeader("X-Device-Provisioning-Secret", CONFIG.provisioningSecret);
  }
}

void setupAudioHardware() {
  i2s_config_t micConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512
  };
  i2s_pin_config_t micPins = {
    .bck_io_num = MIC_SCK,
    .ws_io_num = MIC_WS,
    .data_out_num = -1,
    .data_in_num = MIC_SD
  };
  i2s_driver_install(I2S_NUM_0, &micConfig, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &micPins);

  audioOut = new AudioOutputI2S(1);
  audioOut->SetPinout(AMP_BCLK, AMP_LRC, AMP_DIN);
  audioOut->SetGain(1.0);
  mp3 = new AudioGeneratorMP3();
  Serial.println("[AUDIO] Mic and speaker ready");
}

void playMP3FromURL(String url) {
  if (!mp3 || !audioOut || url.length() == 0) return;

  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();

  if (!http.begin(client, url)) return;
  http.addHeader("User-Agent", "Eldora-Core");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[AUDIO] MP3 download failed HTTP %d\n", code);
    http.end();
    return;
  }

  int len = http.getSize();
  if (len <= 0) {
    http.end();
    return;
  }

  uint8_t* mp3Buffer = (uint8_t*)ps_malloc(len);
  if (!mp3Buffer) {
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  int bytesRead = 0;
  while (http.connected() && bytesRead < len) {
    if (stream->available()) {
      bytesRead += stream->readBytes(&mp3Buffer[bytesRead], len - bytesRead);
    }
    delay(1);
  }
  http.end();

  AudioFileSourcePROGMEM* memSource = new AudioFileSourcePROGMEM(mp3Buffer, len);
  AudioFileSourceID3* id3 = new AudioFileSourceID3(memSource);
  currentState = SPEAKING;
  renderDisplay();

  i2s_stop(I2S_NUM_0);
  if (mp3->begin(id3, audioOut)) {
    while (mp3->isRunning()) {
      if (!mp3->loop()) mp3->stop();
      delay(1);
    }
  }
  i2s_start(I2S_NUM_0);

  delete id3;
  delete memSource;
  free(mp3Buffer);
  voiceCooldownUntil = millis() + VOICE_FEEDBACK_COOLDOWN_MS;
  currentState = ONLINE;
  renderDisplay();
}

void sendVoiceAudio() {
  if (WiFi.status() != WL_CONNECTED || currentAudioSize < VOICE_MIN_RECORD_BYTES) {
    currentAudioSize = 0;
    voiceCooldownUntil = millis() + VOICE_NOISE_COOLDOWN_MS;
    currentState = WiFi.status() == WL_CONNECTED ? ONLINE : ERROR_STATE;
    renderDisplay();
    return;
  }

  currentState = THINKING;
  renderDisplay();

  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();

  String url = String(CONFIG.backendUrl) + "/voice/device/process-audio";
  if (!http.begin(client, url)) {
    currentAudioSize = 0;
    voiceCooldownUntil = millis() + VOICE_NOISE_COOLDOWN_MS;
    currentState = ONLINE;
    renderDisplay();
    return;
  }

  http.addHeader("Content-Type", "application/octet-stream");
  addDeviceHeaders(http);
  http.setTimeout(45000);

  int code = http.POST(audioBuffer, currentAudioSize);
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<768> doc;
    if (!deserializeJson(doc, payload)) {
      const char* audioUrl = doc["data"]["audioUrl"] | "";
      const char* message = doc["data"]["message"] | "";
      if (String(message).length() > 0) showCoreMessage("ELDORA", String(message));
      if (String(audioUrl).length() > 0) playMP3FromURL(String(audioUrl));
    }
  } else {
    Serial.printf("[VOICE] Audio failed HTTP %d\n", code);
  }

  http.end();
  currentAudioSize = 0;
  if (currentState != ONLINE) {
    voiceCooldownUntil = millis() + VOICE_NOISE_COOLDOWN_MS;
    currentState = ONLINE;
  }
  renderDisplay();
}

void audioTask(void* pv) {
  const int chunkSize = 512;
  int32_t i2sRawBuffer[chunkSize];
  size_t bytesRead;
  uint32_t loudSamples = 0;
  uint32_t silenceSamples = 0;
  uint32_t recordedSamples = 0;
  bool speechStarted = false;

  for (;;) {
    if (canAutoListen()) {
      esp_err_t result = i2s_read(I2S_NUM_0, i2sRawBuffer, sizeof(i2sRawBuffer), &bytesRead, portMAX_DELAY);
      if (result == ESP_OK && bytesRead > 0) {
        int samples = bytesRead / 4;
        for (int i = 0; i < samples; i++) {
          int16_t pcmSample = (int16_t)(i2sRawBuffer[i] >> 16);
          micVolume = abs(pcmSample);

          if (!speechStarted) {
            if (micVolume >= SILENCE_THRESHOLD) {
              loudSamples++;
            } else if (loudSamples > 0) {
              loudSamples--;
            }

            if (loudSamples >= VOICE_START_SAMPLES) {
              speechStarted = true;
              currentAudioSize = 0;
              silenceSamples = 0;
              recordedSamples = 0;
            }
          }

          if (speechStarted) {
            if (currentAudioSize < audioBufferSize - 2) {
              memcpy(&audioBuffer[currentAudioSize], &pcmSample, 2);
              currentAudioSize += 2;
              recordedSamples++;
            }
            silenceSamples = micVolume < SILENCE_THRESHOLD ? silenceSamples + 1 : 0;
          }
        }

        if (speechStarted && (currentAudioSize >= audioBufferSize - 2 || silenceSamples > VOICE_END_SILENCE_SAMPLES)) {
          speechStarted = false;
          if (recordedSamples >= VOICE_MIN_SPEECH_SAMPLES) {
            sendVoiceAudio();
          } else {
            currentAudioSize = 0;
            voiceCooldownUntil = millis() + VOICE_NOISE_COOLDOWN_MS;
            currentState = ONLINE;
            renderDisplay();
          }
          loudSamples = 0;
          silenceSamples = 0;
          recordedSamples = 0;
        }
      }
    } else {
      delay(100);
      loudSamples = 0;
      silenceSamples = 0;
      recordedSamples = 0;
      speechStarted = false;
    }
  }
}

bool connectToWifi(String ssid, String password, unsigned long timeoutMs) {
  if (ssid.length() == 0) {
    Serial.println("[WIFI] SSID is empty");
    currentState = ERROR_STATE;
    renderDisplay();
    return false;
  }

  currentState = WIFI_CONNECTING;
  renderDisplay();

  WiFi.disconnect();
  delay(300);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.printf("[WIFI] Connecting to %s", ssid.c_str());
  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected: %s\n", WiFi.localIP().toString().c_str());
    currentState = ONLINE;
    ensureLocalServer();
    startMdns();
    renderDisplay();
    return true;
  }

  Serial.println("[WIFI] Connection failed");
  currentState = ERROR_STATE;
  renderDisplay();
  return false;
}

void sendCorsHeaders() {
  provisioningServer.sendHeader("Access-Control-Allow-Origin", "*");
  provisioningServer.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  provisioningServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

template <typename TDocument>
void sendJson(int statusCode, TDocument& doc) {
  String response;
  serializeJson(doc, response);
  sendCorsHeaders();
  provisioningServer.send(statusCode, "application/json", response);
}

void handleProvisioningInfo() {
  StaticJsonDocument<384> doc;
  doc["productName"] = CONFIG.productName;
  doc["deviceKey"] = CONFIG.deviceKey;
  doc["pairingToken"] = localPairingToken;
  doc["firmwareVersion"] = CONFIG.firmwareVersion;
  doc["setupSsid"] = provisioningSsid();
  doc["setupIp"] = "192.168.4.1";
  doc["hasWifi"] = hasWifiCredentials();
  doc["wifiSsid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  if (WiFi.status() == WL_CONNECTED) doc["wifiRssi"] = WiFi.RSSI();
  doc["localIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "192.168.4.1";
  int batteryLevel = readBatteryLevel();
  if (batteryLevel >= 0) doc["batteryLevel"] = batteryLevel;
  doc["isCharging"] = false;
  sendJson(200, doc);
}

void handleProvisioningOptions() {
  sendCorsHeaders();
  provisioningServer.send(204);
}

bool wifiNetworkAlreadyAdded(JsonArray networks, const String& ssid) {
  for (JsonObject network : networks) {
    const char* existingSsid = network["ssid"];
    if (existingSsid && ssid == existingSsid) return true;
  }

  return false;
}

void handleWifiScan() {
  StaticJsonDocument<4096> doc;
  JsonArray networks = doc.createNestedArray("networks");

  int scanCount = WiFi.scanNetworks(false, true);
  int added = 0;

  if (scanCount > 0) {
    for (int i = 0; i < scanCount && added < 15; i++) {
      String ssid = WiFi.SSID(i);
      ssid.trim();
      if (ssid.length() == 0) continue;
      if (wifiNetworkAlreadyAdded(networks, ssid)) continue;

      JsonObject network = networks.createNestedObject();
      network["ssid"] = ssid;
      network["rssi"] = WiFi.RSSI(i);
      network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      network["channel"] = WiFi.channel(i);
      added++;
    }
  }

  doc["success"] = true;
  doc["count"] = added;
  WiFi.scanDelete();
  sendJson(200, doc);
}

void handleProvisionWifi() {
  String ssid;
  String password;

  if (provisioningServer.hasArg("plain")) {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, provisioningServer.arg("plain"));
    if (!error) {
      ssid = doc["ssid"] | "";
      password = doc["password"] | "";
    }
  } else {
    ssid = provisioningServer.arg("ssid");
    password = provisioningServer.arg("password");
  }

  ssid.trim();
  if (ssid.length() == 0) {
    StaticJsonDocument<128> doc;
    doc["success"] = false;
    doc["message"] = "SSID is required";
    sendJson(400, doc);
    return;
  }

  pendingWifiSsid = ssid;
  pendingWifiPass = password;
  shouldApplyProvisionedWifi = true;

  StaticJsonDocument<192> doc;
  doc["success"] = true;
  doc["message"] = "WiFi received, hub is connecting";
  doc["ssid"] = ssid;
  sendJson(200, doc);
}

void stopProvisioningMode() {
  if (!provisioningMode) return;

  WiFi.softAPdisconnect(true);
  provisioningMode = false;
  Serial.println("[SETUP] Provisioning stopped");
}

void ensureLocalServer() {
  if (localServerStarted) return;
  provisioningServer.on("/", HTTP_GET, handleProvisioningInfo);
  provisioningServer.on("/status", HTTP_GET, handleProvisioningInfo);
  provisioningServer.on("/wifi", HTTP_OPTIONS, handleProvisioningOptions);
  provisioningServer.on("/wifi", HTTP_POST, handleProvisionWifi);
  provisioningServer.on("/wifi/scan", HTTP_OPTIONS, handleProvisioningOptions);
  provisioningServer.on("/wifi/scan", HTTP_GET, handleWifiScan);
  provisioningServer.begin();
  localServerStarted = true;
  Serial.println("[LOCAL] HTTP server started on port 80");
}

void startProvisioningMode() {
  currentState = PROVISIONING;
  provisioningMode = true;

  WiFi.disconnect(true);
  delay(300);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(provisioningSsid().c_str(), provisioningPassword().c_str());
  ensureLocalServer();

  Serial.printf("[SETUP] AP SSID: %s\n", provisioningSsid().c_str());
  Serial.printf("[SETUP] AP PASS: %s\n", provisioningPassword().c_str());
  Serial.println("[SETUP] GET  http://192.168.4.1/wifi/scan to list nearby WiFi networks");
  Serial.println("[SETUP] POST http://192.168.4.1/wifi with {\"ssid\":\"...\",\"password\":\"...\"}");
  renderDisplay();
}

void applyProvisionedWifi() {
  shouldApplyProvisionedWifi = false;
  String nextSsid = pendingWifiSsid;
  String nextPassword = pendingWifiPass;
  pendingWifiSsid = "";
  pendingWifiPass = "";

  stopProvisioningMode();

  if (connectToWifi(nextSsid, nextPassword, WIFI_CONNECT_TIMEOUT_MS)) {
    saveWifiCredentials(nextSsid, nextPassword);
    sendHeartbeat();
    return;
  }

  startProvisioningMode();
}

bool sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();

  String url = String(CONFIG.backendUrl) + "/iot/heartbeat";
  if (!http.begin(client, url)) return false;

  http.addHeader("Content-Type", "application/json");
  addDeviceHeaders(http);
  http.setTimeout(5000);

  StaticJsonDocument<384> doc;
  int batteryLevel = readBatteryLevel();
  if (batteryLevel >= 0) doc["batteryLevel"] = batteryLevel;
  doc["isCharging"] = false;
  doc["wifiSsid"] = WiFi.SSID();
  doc["wifiRssi"] = WiFi.RSSI();
  doc["localIp"] = WiFi.localIP().toString();
  doc["localPairingToken"] = localPairingToken;
  doc["firmwareVersion"] = CONFIG.firmwareVersion;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  http.end();

  Serial.printf("[HB] %s (HTTP %d)\n", code == 200 ? "OK" : "FAIL", code);
  return code == 200;
}

bool ackCommand(String commandId, const char* status, String message) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();

  String url = String(CONFIG.backendUrl) + "/iot/commands/" + commandId + "/ack";
  if (!http.begin(client, url)) return false;

  http.addHeader("Content-Type", "application/json");
  addDeviceHeaders(http);
  http.setTimeout(5000);

  StaticJsonDocument<192> doc;
  doc["status"] = status;
  doc["message"] = message;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  http.end();
  Serial.printf("[CMD] Ack %s (HTTP %d)\n", status, code);
  return code == 200;
}

void showCoreMessage(const String& title, const String& message) {
  // MENGAMANKAN AKSES LAYAR DENGAN MUTEX
  if (xSemaphoreTake(tftMutex, portMAX_DELAY)) {
    tft.fillScreen(lgfx::color565(234, 245, 251));
    tft.setTextDatum(middle_center);
    tft.setTextColor(lgfx::color565(31, 42, 55));
    tft.setTextSize(2);
    tft.drawString(title, 240, 110);
    tft.setTextSize(1);
    tft.setTextColor(lgfx::color565(92, 113, 132));
    tft.drawString(message.substring(0, 42), 240, 165);
    if (message.length() > 42) {
      tft.drawString(message.substring(42, 84), 240, 190);
    }
    xSemaphoreGive(tftMutex); // LEPASKAN KUNCI
  }
  Serial.println("[VOICE] " + title + ": " + message);
}

void handleSpeakOnDoraBot(JsonObject command) {
  String commandId = command["id"] | "";
  String message = command["payload"]["message"] | "Eldora is here. Are you feeling okay?";
  String audioUrl = command["payload"]["audioUrl"] | "";
  if (message.length() == 0) message = "Eldora is here. Are you feeling okay?";
  showCoreMessage("ELDORA CHECK-IN", message);
  if (audioUrl.length() > 0) {
    playMP3FromURL(audioUrl);
    if (commandId.length() > 0) ackCommand(commandId, "applied", "Voice played on DoraBot");
    return;
  }
  if (commandId.length() > 0) ackCommand(commandId, "failed", "Missing voice audio URL");
}

void handleLocalAlarm(JsonObject command) {
  String commandId = command["id"] | "";
  showCoreMessage("ELDORA ALERT", "A caregiver alert has been triggered. Please stay calm.");
  if (commandId.length() > 0) ackCommand(commandId, "applied", "Local alert displayed");
}

void handleConfigureWifi(JsonObject command) {
  String commandId = command["id"] | "";
  String nextSsid = command["payload"]["ssid"] | "";
  String nextPassword = command["payload"]["password"] | "";

  if (commandId.length() == 0 || nextSsid.length() == 0) {
    if (commandId.length() > 0) ackCommand(commandId, "failed", "Invalid WiFi command");
    return;
  }

  currentState = APPLYING_WIFI;
  renderDisplay();
  Serial.println("[CMD] Configure WiFi: " + nextSsid);

  String previousSsid = activeWifiSsid;
  String previousPassword = activeWifiPass;

  if (connectToWifi(nextSsid, nextPassword, WIFI_CONNECT_TIMEOUT_MS)) {
    saveWifiCredentials(nextSsid, nextPassword);
    sendHeartbeat();
    ackCommand(commandId, "applied", "WiFi connected");
    return;
  }

  connectToWifi(previousSsid, previousPassword, WIFI_CONNECT_TIMEOUT_MS);
  ackCommand(commandId, "failed", "WiFi connection failed");
}

void pollCommands() {
  if (WiFi.status() != WL_CONNECTED || currentState == THINKING || currentState == SPEAKING) return;

  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();

  String url = String(CONFIG.backendUrl) + "/iot/commands";
  if (!http.begin(client, url)) return;

  addDeviceHeaders(http);
  http.setTimeout(5000);

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonArray commands = doc["data"].as<JsonArray>();
      for (JsonObject command : commands) {
        String commandType = command["commandType"] | "";
        if (commandType == "configure_wifi") {
          handleConfigureWifi(command);
        } else if (commandType == "speak_on_dorabot" || commandType == "speak_on_core") {
          handleSpeakOnDoraBot(command);
        } else if (commandType == "activate_local_alarm") {
          handleLocalAlarm(command);
        }
      }
    } else {
      Serial.println("[CMD] JSON parse failed");
    }
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.printf("\n[SYS] --- %s ---\n", CONFIG.productName);

  // INISIALISASI MUTEX SEBELUM INIT LAYAR
  tftMutex = xSemaphoreCreateMutex();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  pinMode(VOICE_BUTTON_PIN, INPUT_PULLUP);
  
  tft.init();
  tft.setRotation(3);
  tft.invertDisplay(true);
  renderDisplay();

  if (BATTERY_ADC_PIN >= 0) {
    analogReadResolution(12);
    pinMode(BATTERY_ADC_PIN, INPUT);
  }

  Serial.printf("[CFG] Backend: %s\n", CONFIG.backendUrl);
  Serial.printf("[CFG] Device Key: %.8s...\n", CONFIG.deviceKey);

  preferences.begin(PREF_NAMESPACE, false);

  audioBuffer = (uint8_t*)ps_malloc(audioBufferSize);
  if (audioBuffer) {
    setupAudioHardware();
    xTaskCreatePinnedToCore(audioTask, "Audio", 10000, NULL, 1, NULL, 0);
  } else {
    Serial.println("[AUDIO] PSRAM allocation failed");
  }

  loadWifiCredentials();
  loadPairingToken();

  if (hasWifiCredentials() && connectToWifi(activeWifiSsid, activeWifiPass, WIFI_CONNECT_TIMEOUT_MS)) {
    sendHeartbeat();
  } else {
    startProvisioningMode();
  }
}

void loop() {
  unsigned long now = millis();

  if (localServerStarted) {
    provisioningServer.handleClient();
  }

  if (provisioningMode) {
    if (shouldApplyProvisionedWifi) {
      applyProvisionedWifi();
    }
    renderDisplay();
    vTaskDelay(1);
    return;
  }

  if (currentState == ONLINE && audioBuffer && WiFi.status() == WL_CONNECTED && millis() >= voiceCooldownUntil) {
    currentState = LISTENING;
  }

  static unsigned long lastHeartbeat = 0;
  if (now - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    if (!sendHeartbeat()) {
      currentState = ERROR_STATE;
    }
  }

  static unsigned long lastCommandPoll = 0;
  if (now - lastCommandPoll > COMMAND_POLL_INTERVAL_MS) {
    lastCommandPoll = now;
    pollCommands();
  }

  renderDisplay();
  vTaskDelay(1);
}
