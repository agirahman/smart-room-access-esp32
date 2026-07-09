/**
 * Smart Door Lock — ESP32-CAM [FIXED]
 * 
 * FIX untuk masalah frame lag:
 * ✓ Discard 3-5 frame setelah deteksi RFID
 * ✓ Delay optimal sebelum capture
 * ✓ Clear buffer dengan proper
 * 
 * Alur:
 *   1. Kartu RFID ditap → baca UID
 *   2. [NEW] Buang 3 frame untuk "reset" buffer
 *   3. Ambil foto fresh via OV2640
 *   4. POST ke backend: {uid, room, photo} + header X-API-KEY
 *   5. Tunggu response JSON: {granted: true/false, user: {name, role}}
 *   6. Buka relay jika granted = true (auto-lock setelah 5 detik)
 */

#include "config.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include "esp_camera.h"
#include "time.h"

// ─── Camera Pins (AI-Thinker ESP32-CAM) ──────────────────────────────────────
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ─── NTP ─────────────────────────────────────────────────────────────────────
#define NTP_SERVER  "pool.ntp.org"
#define GMT_OFFSET  25200   // WIB = UTC+7
#define DAYLIGHT    0

// ─── Instances ────────────────────────────────────────────────────────────────
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

// ─── State ───────────────────────────────────────────────────────────────────
bool isUnlocked      = false;
unsigned long unlockTime = 0;

// ─────────────────────────────────────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────────────────────────────────────

String uidToString(byte* uid, byte len) {
  String s = "";
  for (byte i = 0; i < len; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
    if (i < len - 1) s += " ";
  }
  s.toUpperCase();
  return s;
}

String getTimestamp() {
  struct tm t;
  if (!getLocalTime(&t)) return "1970-01-01T00:00:00";
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
  return String(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// CAMERA
// ─────────────────────────────────────────────────────────────────────────────

bool initCamera() {
  camera_config_t cfg;
  cfg.ledc_channel  = LEDC_CHANNEL_0;
  cfg.ledc_timer    = LEDC_TIMER_0;
  cfg.pin_d0        = Y2_GPIO_NUM;
  cfg.pin_d1        = Y3_GPIO_NUM;
  cfg.pin_d2        = Y4_GPIO_NUM;
  cfg.pin_d3        = Y5_GPIO_NUM;
  cfg.pin_d4        = Y6_GPIO_NUM;
  cfg.pin_d5        = Y7_GPIO_NUM;
  cfg.pin_d6        = Y8_GPIO_NUM;
  cfg.pin_d7        = Y9_GPIO_NUM;
  cfg.pin_xclk      = XCLK_GPIO_NUM;
  cfg.pin_pclk      = PCLK_GPIO_NUM;
  cfg.pin_vsync     = VSYNC_GPIO_NUM;
  cfg.pin_href      = HREF_GPIO_NUM;
  cfg.pin_sccb_sda  = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl  = SIOC_GPIO_NUM;
  cfg.pin_pwdn      = PWDN_GPIO_NUM;
  cfg.pin_reset     = RESET_GPIO_NUM;
  cfg.xclk_freq_hz  = 20000000;
  cfg.pixel_format  = PIXFORMAT_JPEG;
  cfg.frame_size    = CAM_FRAMESIZE;
  cfg.jpeg_quality  = CAM_QUALITY;
  cfg.fb_count      = 1;

  if (esp_camera_init(&cfg) != ESP_OK) {
    Serial.println("[CAM] Init failed!");
    return false;
  }

  // Buang 2 frame awal (sering gelap/noise)
  delay(300);
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  Serial.println("[CAM] Ready");
  return true;
}

/**
 * [FIX] Discard beberapa frame untuk reset buffer camera
 * Ketika kartu RFID ditap, buffer mungkin masih punya frame lama
 * Solusi: buang 3 frame, ini akan "memaksa" sensor ambil frame baru
 */
void discardOldFrames(int count = 3) {
  Serial.printf("[CAM] Discarding %d old frames...\n", count);
  for (int i = 0; i < count; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      esp_camera_fb_return(fb);
      delay(33);  // ~30ms, close to frame rate (33ms = ~30fps)
    }
  }
  Serial.println("[CAM] Frame buffer primed ✓");
}

camera_fb_t* capturePhoto() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { 
    Serial.println("[CAM] Capture failed"); 
    return nullptr; 
  }
  Serial.printf("[CAM] Captured %d bytes\n", fb->len);
  return fb;
}

// ─────────────────────────────────────────────────────────────────────────────
// RELAY
// ─────────────────────────────────────────────────────────────────────────────

void lockDoor() {
  digitalWrite(RELAY_PIN, !RELAY_ACTIVE);
  isUnlocked = false;
  Serial.println("[RELAY] Locked");
}

void unlockDoor() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE);
  isUnlocked  = true;
  unlockTime  = millis();
  Serial.println("[RELAY] Unlocked");
}

// Beep pendek — konfirmasi kartu terdeteksi
void beepTap() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE);
  delay(80);
  digitalWrite(RELAY_PIN, !RELAY_ACTIVE);
  delay(80);
}

// 3x beep cepat — akses ditolak
void beepDenied() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(RELAY_PIN, RELAY_ACTIVE);
    delay(80);
    digitalWrite(RELAY_PIN, !RELAY_ACTIVE);
    delay(100);
  }
}

// 1x beep panjang — akses diterima
void beepGranted() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE);
  delay(300);
  digitalWrite(RELAY_PIN, !RELAY_ACTIVE);
  delay(100);
}

// ─────────────────────────────────────────────────────────────────────────────
// BACKEND REQUEST
// Kirim UID + foto ke backend, tunggu response JSON
// Return: true jika backend menjawab granted=true
// ─────────────────────────────────────────────────────────────────────────────

bool sendAccessRequest(String uid, camera_fb_t* photo) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] No WiFi — akses ditolak");
    return false;
  }

  // ── Build multipart body di memory ────────────────────────────────────────
  String boundary = "----ESP32Boundary";

  String textParts = "--" + boundary + "\r\n"
    + "Content-Disposition: form-data; name=\"uid\"\r\n\r\n"
    + uid + "\r\n"
    + "--" + boundary + "\r\n"
    + "Content-Disposition: form-data; name=\"room\"\r\n\r\n"
    + String(ROOM_NAME) + "\r\n";

  bool hasPhoto = (photo && photo->buf && photo->len > 0);
  String photoHead = "";
  if (hasPhoto) {
    photoHead = "--" + boundary + "\r\n"
      + "Content-Disposition: form-data; name=\"photo\"; filename=\"access.jpg\"\r\n"
      + "Content-Type: image/jpeg\r\n\r\n";
  }
  String closing = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = textParts.length() + photoHead.length()
                  + (hasPhoto ? photo->len : 0) + closing.length();

  uint8_t* bodyBuf = (uint8_t*)malloc(totalLen);
  if (!bodyBuf) {
    Serial.println("[HTTP] malloc gagal — skip foto, kirim JSON");
    // Fallback JSON tanpa foto
    WiFiClient c2;
    HTTPClient h2;
    h2.begin(c2, SERVER_URL); 
    h2.setTimeout(SERVER_TIMEOUT);
    h2.addHeader("Content-Type", "application/json");
    h2.addHeader("X-API-KEY", API_KEY);
    String j = "{\"uid\":\"" + uid + "\",\"room\":\"" + String(ROOM_NAME) + "\"}";
    int rc = h2.POST(j); 
    h2.end();
    return rc == 200;
  }

  size_t pos = 0;
  memcpy(bodyBuf + pos, textParts.c_str(), textParts.length()); 
  pos += textParts.length();
  if (hasPhoto) {
    memcpy(bodyBuf + pos, photoHead.c_str(), photoHead.length()); 
    pos += photoHead.length();
    memcpy(bodyBuf + pos, photo->buf, photo->len);               
    pos += photo->len;
  }
  memcpy(bodyBuf + pos, closing.c_str(), closing.length());

  // ── POST dengan retry 1x untuk handle cold start Cloud Run ───────────────
  int code = -1;
  for (int attempt = 1; attempt <= 2 && code == -1; attempt++) {
    if (attempt > 1) {
      Serial.println("[HTTP] Retry...");
      delay(2000);
    }
    WiFiClient client;
    HTTPClient http;
    http.begin(client, SERVER_URL);
    http.setTimeout(SERVER_TIMEOUT);
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http.addHeader("X-API-KEY", API_KEY);
    code = http.POST(bodyBuf, totalLen);
    Serial.printf("[HTTP] Status: %d\n", code);

    if (code > 0) {
      String response = http.getString();
      http.end();
      free(bodyBuf);
      Serial.println("[HTTP] Response: " + response);

      StaticJsonDocument<512> resDoc;
      if (deserializeJson(resDoc, response)) {
        Serial.println("[JSON] Parse error");
        return false;
      }
      String status  = resDoc["data"]["status"]  | "denied";
      String message = resDoc["data"]["message"] | "No message";
      bool granted   = (status == "allowed");
      Serial.printf("[AUTH] %s — %s\n",
        granted ? "GRANTED ✅" : "DENIED ❌", message.c_str());
      return granted;
    }
    http.end();
  }

  free(bodyBuf);
  Serial.println("[HTTP] Gagal setelah retry");
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println("\n============================");
  Serial.println("  Smart Door Lock - ESP32CAM");
  Serial.println("  [FIXED] Real-time capture");
  Serial.println("============================");

  // Relay init
  pinMode(RELAY_PIN, OUTPUT);
  lockDoor();

  // RFID init — HSPI dengan pin custom
  SPI.begin(14, 15, 12, 13);  // SCK=14, MISO=15, MOSI=12, SS=13
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);  // Clone chip (0x17) butuh gain max
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[RFID] Version: 0x%02X — %s\n", ver,
    (ver == 0x00 || ver == 0xFF) ? "ERROR! Cek wiring" : "Ready");

  // Camera init
  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed!");
    while (true) delay(1000);
  }

  // WiFi
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
    configTime(GMT_OFFSET, DAYLIGHT, NTP_SERVER);
    Serial.println("[NTP] Time synced");
  } else {
    Serial.println("\n[WiFi] GAGAL — sistem jalan offline, foto tidak dikirim");
  }

  Serial.println("[READY] Tap kartu RFID...\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP [UPDATED]
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
  // Auto-lock setelah timeout
  if (isUnlocked && (millis() - unlockTime >= UNLOCK_DURATION)) {
    Serial.println("[AUTO-LOCK] Mengunci...");
    lockDoor();
  }

  // Tunggu kartu RFID
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }

  String uid = uidToString(rfid.uid.uidByte, rfid.uid.size);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  Serial.println("─────────────────────────");
  Serial.println("[RFID] UID: " + uid);

  // Beep konfirmasi tap kartu
  beepTap();

  // [FIX] LANGKAH PENTING: Discard frame buffer yang lama
  // Ini memastikan foto yang diambil adalah fresh, bukan dari tap sebelumnya
  discardOldFrames(3);

  Serial.println("[CAM] Mengambil foto...");

  // Ambil foto (sekarang sudah fresh)
  camera_fb_t* photo = capturePhoto();

  // Kirim ke backend → tunggu response
  Serial.println("[HTTP] Mengirim ke backend...");
  bool granted = sendAccessRequest(uid, photo);

  // Buka kunci atau beep denied
  if (granted) {
    beepGranted();    // 1x beep panjang
    unlockDoor();     // relay ON → solenoid buka
  } else {
    beepDenied();     // 3x beep cepat
  }

  // Bebaskan memori foto
  if (photo) {
    esp_camera_fb_return(photo);
    photo = nullptr;
  }

  Serial.println("[READY] Menunggu kartu berikutnya...\n");
  delay(1500);  // debounce
}
