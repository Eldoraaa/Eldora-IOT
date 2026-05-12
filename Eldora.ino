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
#define ELDORA_FIRMWARE_VERSION "0.4.1"

struct EldoraConfig {
  const char* productName;
  const char* backendUrl;
  const char* deviceKey;
  const char* firmwareVersion;
};

const EldoraConfig CONFIG = {
  ELDORA_PRODUCT_NAME,
  ELDORA_BACKEND_URL,
  ELDORA_DEVICE_KEY,
  ELDORA_FIRMWARE_VERSION,
};

// TFT pins.
#define TFT_SCK 12
#define TFT_MOSI 11
#define TFT_CS 10
#define TFT_DC 9
#define TFT_RST 8
#define TFT_BL 14

// Timers.
#define HEARTBEAT_INTERVAL_MS 30000UL
#define COMMAND_POLL_INTERVAL_MS 15000UL
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

enum HubState { BOOTING, PROVISIONING, WIFI_CONNECTING, ONLINE, APPLYING_WIFI, ERROR_STATE };
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

String activeWifiSsid;
String activeWifiPass;
String pendingWifiSsid;
String pendingWifiPass;
String localPairingToken;
bool provisioningMode = false;
bool shouldApplyProvisionedWifi = false;
bool localServerStarted = false;
bool mdnsStarted = false;

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
    case ERROR_STATE:
      return lgfx::color565(235, 95, 95);
    default:
      return lgfx::color565(255, 138, 122);
  }
}

const uint8_t* currentFrame() {
  if (currentState == ONLINE) return img_speak;
  if (currentState == PROVISIONING || currentState == WIFI_CONNECTING || currentState == APPLYING_WIFI) return img_listen;
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
  http.addHeader("X-Device-Key", CONFIG.deviceKey);
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
  http.addHeader("X-Device-Key", CONFIG.deviceKey);
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
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();

  String url = String(CONFIG.backendUrl) + "/iot/commands";
  if (!http.begin(client, url)) return;

  http.addHeader("X-Device-Key", CONFIG.deviceKey);
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

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
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

  // Clear Preference
  preferences.clear(); 
  Serial.println("[SYS] FACTORY RESET BERHASIL!");
  // -------------------------------------

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
