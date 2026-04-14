// ==========================================
// ELDORA CARE — ESP32 Firmware
// Versi baru tanpa LLM/STT/TTS/Audio
// Animasi LCD dari ABot.ino tetap dipertahankan
// ==========================================

#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <LovyanGFX.hpp>
#include <ArduinoJson.h>
#include <math.h>
#include "images_data.h"

// ==========================================
// 1. KONFIGURASI
// ==========================================
const char* PRODUCT_NAME  = "ELDORA_CARE";
const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASS     = "YOUR_PASSWORD";
const char* BACKEND_URL   = "http://192.168.1.100:3000";  // IP server lokal
const char* DEVICE_KEY    = "YOUR_DEVICE_KEY_FROM_SEED";  // dari output seed

// Pin TFT (identik dengan ABot.ino)
#define TFT_SCK   12
#define TFT_MOSI  11
#define TFT_CS    10
#define TFT_DC    9
#define TFT_RST   8
#define TFT_BL    14

// Sensor & Tombol
#define DHT_PIN   4
#define BTN_PIN   1

// Timing (ms)
#define HEARTBEAT_INTERVAL_MS  30000UL   // 30 detik
#define DHT_INTERVAL_MS        10000UL   // 10 detik
#define SUCCESS_DISPLAY_MS     2000UL    // tampil success/error 2 detik
#define DOUBLE_PRESS_WINDOW    400UL     // jendela deteksi double press
#define LONG_PRESS_THRESHOLD   3000UL    // 3 detik = emergency
#define DHT_ANOMALY_COOLDOWN   300000UL  // 5 menit cooldown per anomaly

// Retry
#define MAX_RETRIES     3
#define RETRY_BASE_MS   1000UL

// Threshold anomali
#define TEMP_HIGH_THRESHOLD    35.0f
#define TEMP_LOW_THRESHOLD     18.0f
#define HUM_HIGH_THRESHOLD     90.0f

// ==========================================
// 2. STATE MACHINE
// ==========================================
enum State { IDLE, SENDING, SUCCESS, ERROR_STATE };
State currentState = IDLE;
State lastState    = IDLE;

unsigned long stateChangeTime = 0;

void setState(State s) {
  if (s != currentState) {
    const char* names[] = { "IDLE", "SENDING", "SUCCESS", "ERROR" };
    Serial.printf("[%lu] [STATE] %s -> %s\n", millis(), names[currentState], names[s]);
    currentState    = s;
    stateChangeTime = millis();
  }
}

// ==========================================
// 3. HARDWARE — TFT (identik dengan ABot.ino)
// ==========================================
class LGFX_ESP32S3 : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
public:
  LGFX_ESP32S3() {
    auto cfg_b = _bus.config();
    cfg_b.spi_host  = SPI2_HOST;
    cfg_b.pin_sclk  = TFT_SCK;
    cfg_b.pin_mosi  = TFT_MOSI;
    cfg_b.pin_miso  = -1;
    cfg_b.pin_dc    = TFT_DC;
    cfg_b.freq_write = 15000000;
    _bus.config(cfg_b);
    _panel.setBus(&_bus);

    auto cfg_p = _panel.config();
    cfg_p.pin_cs     = TFT_CS;
    cfg_p.pin_rst    = TFT_RST;
    cfg_p.panel_width  = 320;
    cfg_p.panel_height = 480;
    cfg_p.rgb_order    = true;
    _panel.config(cfg_p);
    setPanel(&_panel);
  }
};
LGFX_ESP32S3 tft;

DHT dht(DHT_PIN, DHT11);

// ==========================================
// 4. ANIMASI VISUALIZER
// ==========================================
// Bar positions — identik dengan kode lama ABot.ino
// Diaktifkan dengan animasi sintetis (tanpa mikrofon)
void drawVisualizer() {
  const int base_x    = 180;
  const int base_y    = 300;
  const int bar_width = 15;
  const int spacing   = 10;
  const int max_h     = 60;

  if (currentState == IDLE || currentState == ERROR_STATE) {
    // Bersihkan semua bar
    for (int i = 0; i < 5; i++) {
      int x_pos = base_x + (i * (bar_width + spacing));
      tft.fillRect(x_pos, base_y - max_h, bar_width, max_h, TFT_BLACK);
    }
    return;
  }

  // Animasi sintetis menggunakan gelombang sinus
  for (int i = 0; i < 5; i++) {
    float phase       = millis() / 200.0f + i * 0.8f;
    int   synth_vol   = (int)((sinf(phase) + 1.0f) * 5000.0f);  // 0–10000
    int   h           = map(synth_vol, 0, 10000, 5, max_h);
    h                 = constrain(h, 5, max_h);
    int x_pos         = base_x + (i * (bar_width + spacing));

    tft.fillRect(x_pos, base_y - h,     bar_width, h,          lgfx::color565(0, 255 - (i * 30), 255));
    tft.fillRect(x_pos, base_y - max_h, bar_width, max_h - h,  TFT_BLACK);
  }
}

// ==========================================
// 5. HTTP — Kirim Event ke Backend
// ==========================================
bool sendEventToBackend(const char* eventType, String detail) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi tidak terhubung — event dibatalkan");
    return false;
  }

  setState(SENDING);

  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      unsigned long delayMs = RETRY_BASE_MS * (1UL << (attempt - 1));  // 1s, 2s, 4s
      Serial.printf("[HTTP] Retry %d/%d dalam %lu ms\n", attempt, MAX_RETRIES - 1, delayMs);
      delay(delayMs);
    }

    String url = String(BACKEND_URL) + "/iot/events";
    WiFiClient client;
    HTTPClient http;

    if (!http.begin(client, url)) {
      Serial.println("[HTTP] Gagal begin");
      http.end();
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Key", DEVICE_KEY);
    http.setTimeout(10000);

    StaticJsonDocument<256> doc;
    doc["eventType"] = eventType;
    doc["payload"]["detail"] = detail;
    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    http.end();

    if (code == 200 || code == 201) {
      Serial.printf("[HTTP] Event '%s' OK (attempt %d). HTTP %d\n", eventType, attempt + 1, code);
      setState(SUCCESS);
      return true;
    }
    Serial.printf("[HTTP] Event '%s' GAGAL (attempt %d). HTTP %d\n", eventType, attempt + 1, code);
  }

  setState(ERROR_STATE);
  return false;
}

bool sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(BACKEND_URL) + "/iot/heartbeat";
  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, url)) return false;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);
  http.setTimeout(5000);

  int code = http.POST("");
  http.end();

  Serial.printf("[HB] Heartbeat %s (HTTP %d)\n", code == 200 ? "OK" : "FAIL", code);
  return (code == 200);
}

// ==========================================
// 6. BUTTON — 1 tombol 3 aksi
// ==========================================
static bool   lastBtnState        = HIGH;
static unsigned long pressStart   = 0;
static bool   longPressTriggered  = false;
static unsigned long lastShortRelease = 0;
static bool   pendingAssistance   = false;

void handleButton() {
  bool btn = digitalRead(BTN_PIN);

  // --- Tombol baru ditekan ---
  if (btn == LOW && lastBtnState == HIGH) {
    pressStart          = millis();
    longPressTriggered  = false;
  }

  // --- Tombol sedang ditekan (cek long press) ---
  if (btn == LOW && lastBtnState == LOW) {
    if (!longPressTriggered && (millis() - pressStart >= LONG_PRESS_THRESHOLD)) {
      longPressTriggered = true;
      Serial.println("[BTN] EMERGENCY (long press)");
      sendEventToBackend("emergency", "Tombol darurat ditekan");
    }
  }

  // --- Tombol dilepas ---
  if (btn == HIGH && lastBtnState == LOW) {
    unsigned long duration = millis() - pressStart;

    if (duration < 150UL) {
      // Debounce — abaikan
    } else if (!longPressTriggered && duration < 1500UL) {
      // Short press: cek apakah double click
      if (lastShortRelease > 0 && (millis() - lastShortRelease) < DOUBLE_PRESS_WINDOW) {
        // Double press → service_request
        pendingAssistance = false;
        lastShortRelease  = 0;
        Serial.println("[BTN] Service request (double press)");
        sendEventToBackend("service_request", "Permintaan layanan");
      } else {
        // Catat waktu release, tunggu konfirmasi double
        lastShortRelease  = millis();
        pendingAssistance = true;
      }
    }
    // Long press sudah di-handle saat hold — tidak perlu aksi saat release
  }

  lastBtnState = btn;
}

// Cek pending single press (tunggu jendela double press berlalu)
void checkPendingAssistance() {
  if (pendingAssistance && lastShortRelease > 0 &&
      (millis() - lastShortRelease) > DOUBLE_PRESS_WINDOW) {
    pendingAssistance = false;
    lastShortRelease  = 0;
    Serial.println("[BTN] Assistance request (short press)");
    sendEventToBackend("assistance_request", "Permintaan bantuan");
  }
}

// ==========================================
// 7. DHT — Cek Anomali Sensor
// ==========================================
static unsigned long lastTempAnomalyTime = 0;
static unsigned long lastHumAnomalyTime  = 0;

void checkDHTAnomaly() {
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("[DHT] Gagal baca sensor");
    return;
  }

  Serial.printf("[DHT] Suhu: %.1f°C | Kelembaban: %.1f%%\n", temp, hum);

  // Cek anomali suhu
  if ((temp > TEMP_HIGH_THRESHOLD || temp < TEMP_LOW_THRESHOLD) &&
      (millis() - lastTempAnomalyTime > DHT_ANOMALY_COOLDOWN)) {
    lastTempAnomalyTime = millis();
    String detail = "Suhu abnormal: " + String(temp, 1) + "C";
    Serial.println("[DHT] ANOMALY: " + detail);
    sendEventToBackend("sensor_anomaly", detail);
  }

  // Cek anomali kelembaban
  if (hum > HUM_HIGH_THRESHOLD &&
      (millis() - lastHumAnomalyTime > DHT_ANOMALY_COOLDOWN)) {
    lastHumAnomalyTime = millis();
    String detail = "Kelembaban tinggi: " + String(hum, 1) + "%";
    Serial.println("[DHT] ANOMALY: " + detail);
    sendEventToBackend("sensor_anomaly", detail);
  }
}

// ==========================================
// 8. RENDER DISPLAY
// ==========================================
void renderDisplay() {
  const unsigned char* img;
  if      (currentState == SENDING) img = img_listen;
  else if (currentState == SUCCESS)  img = img_speak;
  else                               img = img_idle;   // IDLE & ERROR_STATE

  tft.drawBitmap(0, 0, img, 480, 320, TFT_BLACK, TFT_WHITE);
  drawVisualizer();
}

// ==========================================
// 9. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.printf("\n[SYS] --- %s ---\n", PRODUCT_NAME);

  // TFT init (identik dengan ABot.ino)
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_WHITE);
  tft.invertDisplay(true);
  Serial.println("[TFT] Ready");

  // Button
  pinMode(BTN_PIN, INPUT_PULLUP);

  // DHT
  dht.begin();
  Serial.println("[DHT] Ready");

  // WiFi
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WIFI] Connected: %s\n", WiFi.localIP().toString().c_str());

  Serial.printf("[CFG] Backend: %s\n", BACKEND_URL);
  Serial.printf("[CFG] Device Key: %.8s...\n", DEVICE_KEY);  // print 8 char pertama saja

  // Kirim heartbeat pertama saat boot
  sendHeartbeat();

  Serial.println("[SYS] System Ready");
}

// ==========================================
// 10. LOOP
// ==========================================
void loop() {
  unsigned long now = millis();

  // --- Heartbeat timer ---
  static unsigned long lastHeartbeat = 0;
  if (now - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    sendHeartbeat();
  }

  // --- DHT timer ---
  static unsigned long lastDHT = 0;
  if (now - lastDHT > DHT_INTERVAL_MS) {
    lastDHT = now;
    checkDHTAnomaly();
  }

  // --- Auto-reset SUCCESS/ERROR ke IDLE ---
  if ((currentState == SUCCESS || currentState == ERROR_STATE) &&
      (now - stateChangeTime > SUCCESS_DISPLAY_MS)) {
    setState(IDLE);
  }

  // --- Button handling ---
  handleButton();
  checkPendingAssistance();

  // --- Render display ---
  renderDisplay();

  vTaskDelay(1);  // beri waktu watchdog
}
