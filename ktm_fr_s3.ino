#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "time.h"
#include <HTTPClient.h>
#include <Adafruit_PN532.h>

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"

// ===========================
// Konfigurasi Pin I2C & LCD 2x16 untuk ESP32-S3
// ===========================
#define I2C_SDA 1
#define I2C_SCL 2
#define LCD_ADDR 0x27

// Alamat I2C: 0x27 (16 kolom, 2 baris)
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// ===========================
// Konfigurasi Pin Modul NFC PN532 (Mode HSU / Hardware Serial1)
// ===========================
// Pin UART mandiri bebas dari Kamera, LCD, dan PIR:
#define PN532_RX_PIN    47  // Hubungkan ke pin TX (SDA) pada modul PN532
#define PN532_TX_PIN    48  // Hubungkan ke pin RX (SCL) pada modul PN532
#define PN532_RESET_PIN 21  // Hubungkan ke pin RST (RSTPD_N) pada PN532

// Inisialisasi Objek PN532 mode HSU
Adafruit_PN532 nfc(PN532_RESET_PIN, &Serial1);

// ===========================
// Konfigurasi Pin Sensor PIR (HC-SR501) & Manajemen Daya
// ===========================
#define PIR_PIN 38

// Durasi aktif standby setelah gerakan terakhir terdeteksi (5 Menit = 300.000 ms)
const unsigned long ACTIVE_TIMEOUT_MS = 5 * 60 * 1000UL;

// Durasi Jendela Face Recognition (FR) setelah Kartu di-tap (8 Detik = 8.000 ms)
const unsigned long FR_DURATION_MS = 8000UL;

// Status sistem: Default mati (false = standby/sleep, true = aktif)
bool isSystemActive = false;
unsigned long lastMotionMillis = 0;

// Status Face Recognition (FR) setelah Tap Kartu
bool isFrActive = false;
unsigned long frStartMillis = 0;
char currentCardUid[32] = "";

// Debounce kartu: kartu yang sama dianggap masih menempel selama masih terbaca
// dalam jeda ini, sehingga /tap tidak dikirim berulang-ulang
const unsigned long CARD_DEBOUNCE_MS = 3000UL;
char lastTapUid[32] = "";
unsigned long lastCardSeenMillis = 0;

// Status deteksi NFC non-blocking (InListPassiveTarget sedang menunggu kartu)
bool nfcArmed = false;

// ===========================
// Konfigurasi WiFi & Backend (Secrets / Git-ignored)
// ===========================
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// NTP Server untuk Sinkronisasi Jam (WIB = UTC+7)
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // GMT+7 (WIB)
const int   daylightOffset_sec = 0;

void startCameraServer();
void setupLedFlash();

// ===========================
// Variabel Status LCD & Wajah
// ===========================
char lastFaceName[32] = "";
char lastFaceTime[16] = "";
char lastFaceScore[16] = "";
unsigned long lastFaceDetectedMillis = 0;
bool isFaceActive = false;

// Fungsi helper untuk mencetak tepat 16 karakter tanpa flicker (menghindari lcd.clear())
void printLcdLine(uint8_t row, const char* format, ...) {
  if (!isSystemActive) return; // Jangan print jika LCD dalam mode mati

  char rawBuf[64];
  char lineBuf[17];

  va_list args;
  va_start(args, format);
  vsnprintf(rawBuf, sizeof(rawBuf), format, args);
  va_end(args);

  // Format agar tepat 16 karakter dengan spasi (padding)
  snprintf(lineBuf, sizeof(lineBuf), "%-16.16s", rawBuf);

  lcd.setCursor(0, row);
  lcd.print(lineBuf);
}

// Fungsi mendapatkan waktu sekarang dari internal RTC/NTP (format HH:MM:SS)
String getCurrentTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "00:00:00";
  }
  char timeStr[16];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  return String(timeStr);
}

// Fungsi kirim notifikasi tap kartu ke Python server secara async di background task
void sendCardTapToPythonAsync(const char* uid) {
  if (WiFi.status() != WL_CONNECTED) return;

  String* pUid = new String(uid);
  xTaskCreate([](void* param) {
    String* cardUid = (String*)param;
    HTTPClient http;
    String url = String("http://") + pythonServerIp + ":" + String(pythonServerPort) + "/tap?uid=" + *cardUid + "&cam=ESP32-S3";
    http.begin(url);
    http.setTimeout(1500);
    int httpCode = http.GET();
    if (httpCode > 0) {
      Serial.printf("[HTTP TAP] Sukses mengirim UID %s ke Python Server (HTTP %d)\n", cardUid->c_str(), httpCode);
    } else {
      Serial.printf("[HTTP TAP WARN] Gagal menghubungi Python Server di %s (%s)\n", url.c_str(), http.errorToString(httpCode).c_str());
    }
    http.end();
    delete cardUid;
    vTaskDelete(NULL);
  }, "http_tap_task", 4096, pUid, 1, NULL);
}

// Fungsi kontrol daya Kamera & Layar LCD berdasarkan Sensor PIR
void setSystemPower(bool enable) {
  isSystemActive = enable;

  if (enable) {
    // Nyalakan Layar LCD & Backlight
    lcd.backlight();
    String nowTime = getCurrentTimeString();
    printLcdLine(0, "Jam : %s", nowTime.c_str());
    printLcdLine(1, "Silakan Tap KTM ");

    Serial.println("\n========================================================");
    Serial.println("🚨 [PIR] GERAKAN TERDETEKSI -> KAMERA & LCD MENYALA!");
    Serial.println("   Status: Aktif selama 5 menit sejak gerakan terakhir.");
    Serial.println("========================================================\n");
  } else {
    // Bersihkan layar dan matikan backlight LCD
    lcd.clear();
    lcd.noBacklight();
    isFaceActive = false;
    isFrActive = false;

    Serial.println("\n========================================================");
    Serial.println("💤 [PIR] 5 MENIT TANPA GERAKAN -> KAMERA & LCD DIMATIKAN!");
    Serial.println("   Status: Mode Standby / Sleep (Menunggu gerakan baru)...");
    Serial.println("========================================================\n");
  }
}

// Fungsi yang dipanggil dari face_handler saat ada wajah terdeteksi dari Python
void onFaceDetected(const char* name, const char* time_str, const char* score, const char* cam) {
  unsigned long now = millis();

  // Jika ada data wajah masuk saat sistem mati, langsung nyalakan sistem
  if (!isSystemActive) {
    setSystemPower(true);
    lastMotionMillis = now;
  }

  // Jika verifikasi FR sedang aktif, tandai FR selesai
  if (isFrActive) {
    isFrActive = false;
    Serial.printf("\n[FR HASIL] Verifikasi Wajah Selesai untuk Kartu UID: %s\n", currentCardUid);
    Serial.printf(" • Hasil Nama : %s\n", name);
    Serial.printf(" • Skor Match : %s\n", score);
    Serial.printf(" • Waktu      : %s\n\n", time_str);
  }

  // Cek apakah data wajah yang baru masuk adalah "Unknown" atau kosong
  bool isIncomingUnknown = (strcasecmp(name, "Unknown") == 0 || strcmp(name, "-") == 0);

  if (isIncomingUnknown) {
    if (isFaceActive && (now - lastFaceDetectedMillis < 5000) && (strcasecmp(lastFaceName, "Unknown") != 0)) {
      return; // Tahan tampilan nama wajah yang terkenali pada LCD
    }
  }

  strncpy(lastFaceName, name, sizeof(lastFaceName) - 1);
  strncpy(lastFaceScore, score, sizeof(lastFaceScore) - 1);

  String tStr = String(time_str);
  int spaceIdx = tStr.indexOf(' ');
  if (spaceIdx >= 0) {
    tStr = tStr.substring(spaceIdx + 1);
  }
  if (tStr.length() == 0 || tStr == "-") {
    tStr = getCurrentTimeString();
  }
  strncpy(lastFaceTime, tStr.c_str(), sizeof(lastFaceTime) - 1);

  lastFaceDetectedMillis = now;
  isFaceActive = true;

  // Langsung tampilkan hasil pengenalan ke LCD secara instan
  printLcdLine(0, "Wkt : %s", lastFaceTime);
  printLcdLine(1, "Nama: %s", lastFaceName);
}

void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.println("\n========================================");
  Serial.println("    [DIAGNOSTIK HARDWARE RESET]        ");
  Serial.println("========================================");
  Serial.print("• Penyebab Booting Terakhir: ");
  
  switch (reason) {
    case ESP_RST_POWERON:   Serial.println("POWER-ON (Normal Dinyalakan)"); break;
    case ESP_RST_EXT:       Serial.println("EXTERNAL RESET (Tombol Reset Ditekan)"); break;
    case ESP_RST_SW:        Serial.println("SOFTWARE RESET (Restart via Kode)"); break;
    case ESP_RST_PANIC:     Serial.println("⚠️ SYSTEM CRASH / PANIC (Error Kode/RAM!)"); break;
    case ESP_RST_INT_WDT:   Serial.println("⚠️ WATCHDOG RESET (CPU Stuck/Hang!)"); break;
    case ESP_RST_BROWNOUT:  Serial.println("🚨 BROWNOUT RESET (TEGANGAN 5V/3.3V DROP/LEMAH!)"); break;
    default:                Serial.printf("KODE LAIN (%d)\n", reason); break;
  }
  Serial.println("========================================\n");

  if (reason == ESP_RST_BROWNOUT) {
    printLcdLine(0, "WARN: BROWNOUT!");
    printLcdLine(1, "Power Drop 5V!");
    delay(2500);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // 1. Inisialisasi Pin Sensor PIR
  pinMode(PIR_PIN, INPUT);

  // 2. Inisialisasi Hardware Serial1 untuk Modul NFC PN532 (HSU Mode)
  Serial1.begin(115200, SERIAL_8N1, PN532_RX_PIN, PN532_TX_PIN);
  // Library membaca frame balasan dengan readBytes(); timeout default 1000 ms membuat loop() tertahan
  Serial1.setTimeout(30);
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("⚠️ WARN: Modul NFC PN532 tidak terdeteksi pada Serial1 (GPIO 47/48)!");
    Serial.println("   Periksa saklar HSU (0 0) dan sambungan kabel.");
  } else {
    Serial.printf("✓ Modul NFC PN532 Terdeteksi! Chip: PN5%02X | FW: %d.%d\n",
                  (versiondata >> 24) & 0xFF, (versiondata >> 16) & 0xFF, (versiondata >> 8) & 0xFF);
    nfc.SAMConfig();
    nfc.setPassiveActivationRetries(0xFF);

    // Optimasi: Atur Receiver Gain (RxGain) ke level Maksimum (43dB)
    // agar kartu smartcard tebal (KTM, Flazz, E-Money, e-KTP) terbaca dengan sangat sensitif & responsif
    uint8_t max_gain_cmd[] = { PN532_COMMAND_RFCONFIGURATION, 0x05, 0x01, 0x01, 0x07 };
    nfc.sendCommandCheckAck(max_gain_cmd, sizeof(max_gain_cmd));
    Serial.println("✓ Antena PN532 dioptimasi: Max RF Receiver Gain (Sensitivitas Tinggi)");
  }

  // 3. Diagnostik Hardware Reset
  printResetReason();

  // 4. Inisialisasi I2C dan LCD 2x16
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000); // 100KHz agar lebih stabil dari noise
  lcd.init();
  lcd.backlight();
  printLcdLine(0, "ESP32-S3 Boot..");
  printLcdLine(1, "Inisialisasi...");

  // 5. Konfigurasi Kamera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_VGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 2;

  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 12;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      config.frame_size = FRAMESIZE_VGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // Inisialisasi Kamera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    printLcdLine(0, "Cam Init GAGAL!");
    printLcdLine(1, "Err: 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  // 6. Menghubungkan ke WiFi
  printLcdLine(0, "Connecting...");
  printLcdLine(1, "%-16.16s", ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  WiFi.setTxPower(WIFI_POWER_17dBm);

  Serial.println("WiFi connecting...");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 60) {
    delay(1000);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, "id.pool.ntp.org", "time.nist.gov");

    printLcdLine(0, "WiFi Terhubung!");
    printLcdLine(1, "IP:%s", WiFi.localIP().toString().c_str());
    delay(2000);
  } else {
    Serial.printf("\nWiFi Gagal terhubung, status=%d\n", WiFi.status());
    printLcdLine(0, "WiFi GAGAL!");
    printLcdLine(1, "Cek SSID/Pass");
    delay(2000);
  }

  // 7. Jalankan Web Server Kamera & Endpoint /face
  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  // 8. SET DEFAULT AWAL: STANDBY (Menunggu gerakan PIR / Tap Kartu)
  setSystemPower(false);
  Serial.println("[SYSTEM] Siap! Default: Kamera & LCD MATI (Menunggu gerakan atau tap KTM)...");
}

void loop() {
  unsigned long currentMillis = millis();

  // ============================================================================
  // 1. BACA SENSOR PIR (HC-SR501 di GPIO 38)
  // ============================================================================
  int pirState = digitalRead(PIR_PIN);
  if (pirState == HIGH) {
    lastMotionMillis = currentMillis;
    if (!isSystemActive) {
      setSystemPower(true); // Bangunkan sistem saat ada orang
    }
  }

  // ============================================================================
  // 2. BACA CARD READER NFC PN532 (HSU GPIO 47 & 48)
  // ============================================================================
  uint8_t uid[10]; // ISO 14443-A UID bisa 4, 7, atau 10 byte
  uint8_t uidLength = 0;
  bool cardDetected = false;
  // Deteksi non-blocking: kirim InListPassiveTarget sekali, lalu cek tiap loop apakah
  // PN532 sudah membalas (balasan hanya datang saat ada kartu ISO 14443-A di medan RF)
  if (!nfcArmed) {
    while (Serial1.available()) Serial1.read(); // Buang sisa byte frame lama (DCS/postamble/ATS)
    nfcArmed = nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A);
  } else if (Serial1.available()) {
    delay(5); // Tunggu frame balasan lengkap (~2 ms @115200)
    cardDetected = nfc.readDetectedPassiveTargetID(uid, &uidLength);
    nfcArmed = false; // Pasang ulang deteksi pada loop berikutnya
  }

  if (cardDetected) {
    String uidCheck = "";
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid[i] < 0x10) uidCheck += "0";
      uidCheck += String(uid[i], HEX);
    }
    uidCheck.toUpperCase();

    // Kartu yang sama masih menempel di reader -> jangan picu ulang FR & /tap
    bool sameCardStillPresent = (strcmp(uidCheck.c_str(), lastTapUid) == 0) &&
                                (currentMillis - lastCardSeenMillis < CARD_DEBOUNCE_MS);
    lastCardSeenMillis = currentMillis;
    if (sameCardStillPresent) {
      cardDetected = false;
      lastMotionMillis = currentMillis;
    } else {
      strncpy(lastTapUid, uidCheck.c_str(), sizeof(lastTapUid) - 1);
    }
  }

  if (cardDetected) {
    String uidClean = "";
    String uidFormatted = "";
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid[i] < 0x10) {
        uidClean += "0";
        uidFormatted += "0";
      }
      uidClean += String(uid[i], HEX);
      uidFormatted += String(uid[i], HEX);
      if (i < uidLength - 1) uidFormatted += " ";
    }
    uidClean.toUpperCase();
    uidFormatted.toUpperCase();
    strncpy(currentCardUid, uidClean.c_str(), sizeof(currentCardUid) - 1);

    // Bangunkan sistem jika sedang standby
    if (!isSystemActive) {
      setSystemPower(true);
    }
    lastMotionMillis = currentMillis;

    // Aktifkan Jendela Waktu Face Recognition (8 Detik)
    isFrActive = true;
    frStartMillis = currentMillis;
    isFaceActive = false; // Reset tampilan wajah sebelumnya

    // Cetak Data Kartu & Durasi FR ke Serial Monitor
    Serial.println("\n========================================================");
    Serial.println("💳 [KTM / KARTU TERDETEKSI DI CARD READER]");
    Serial.printf(" • UID Kartu (Clean)  : %s\n", uidClean.c_str());
    Serial.printf(" • UID Kartu (Format) : %s\n", uidFormatted.c_str());
    Serial.printf(" • Panjang UID        : %d Byte\n", uidLength);
    Serial.printf(" • Durasi FR Aktif    : %.1f Detik (Mulai memindai wajah...)\n", FR_DURATION_MS / 1000.0);
    Serial.println("========================================================\n");

    // Tampilkan di Layar LCD
    printLcdLine(0, "KTM:%-12.12s", currentCardUid);
    printLcdLine(1, "Scan Muka: 8s.. ");

    // Kirim Webhook Tap Kartu ke Python Server (ktm_face_verification.py)
    sendCardTapToPythonAsync(currentCardUid);
  }

  // ============================================================================
  // 3. LOGIKA COUNTDOWN FR & MANAJEMEN TAMPILAN LCD
  // ============================================================================
  if (isSystemActive) {
    // Mode A: Jendela Face Recognition (FR) sedang aktif setelah kartu di-tap
    if (isFrActive) {
      // Cek apakah durasi 8 detik sudah habis
      if (currentMillis - frStartMillis >= FR_DURATION_MS) {
        isFrActive = false;
        Serial.printf("\n[⏱️ FR TIMEOUT] Durasi %.1f detik habis untuk kartu UID %s.\n\n",
                      FR_DURATION_MS / 1000.0, currentCardUid);
        printLcdLine(0, "KTM:%-12.12s", currentCardUid);
        printLcdLine(1, "FR: Waktu Habis!");
        delay(1200);
      } else {
        // Tampilkan Countdown Detik Tersisa pada LCD & Serial tiap 500 ms
        static unsigned long lastFrCountdownMillis = 0;
        if (currentMillis - lastFrCountdownMillis >= 500) {
          lastFrCountdownMillis = currentMillis;
          int remSec = (FR_DURATION_MS - (currentMillis - frStartMillis) + 999) / 1000;

          if (!isFaceActive) {
            printLcdLine(0, "KTM:%-12.12s", currentCardUid);
            printLcdLine(1, "Scan Muka: %ds..", remSec);
            Serial.printf("[FR COUNTDOWN] Sisa durasi verifikasi wajah: %d detik\n", remSec);
          }
        }
      }
    }
    // Mode B: Kondisi Normal setelah FR selesai (menampilkan hasil pengenalan atau jam)
    else {
      // Cek timeout 5 menit tidak ada gerakan
      if (currentMillis - lastMotionMillis >= ACTIVE_TIMEOUT_MS) {
        setSystemPower(false); // Masuk mode sleep/standby
      } else {
        static unsigned long lastUpdateMillis = 0;
        if (currentMillis - lastUpdateMillis >= 500) {
          lastUpdateMillis = currentMillis;

          // Jika ada hasil wajah terdeteksi dalam 5 detik terakhir
          if (isFaceActive && (currentMillis - lastFaceDetectedMillis < 5000)) {
            printLcdLine(0, "Wkt : %s", lastFaceTime);
            printLcdLine(1, "Nama: %s", lastFaceName);
          } else {
            isFaceActive = false;
            String nowTime = getCurrentTimeString();
            printLcdLine(0, "Jam : %s", nowTime.c_str());
            printLcdLine(1, "Silakan Tap KTM ");
          }
        }
      }
    }

    // Reset berkala bus LCD tiap 60 detik (hanya saat aktif)
    static unsigned long lastLcdReset = 0;
    if (currentMillis - lastLcdReset >= 60000) {
      lastLcdReset = currentMillis;
      lcd.init();
      lcd.backlight();
    }
  }

  // ============================================================================
  // 4. DIAGNOSTIK RSSI WIFI TIAP 5 DETIK
  // ============================================================================
  static unsigned long lastDiagMillis = 0;
  if (currentMillis - lastDiagMillis >= 5000) {
    lastDiagMillis = currentMillis;
    if (WiFi.status() == WL_CONNECTED) {
      int rssi = WiFi.RSSI();
      Serial.printf("[DIAGNOSTIK STATS] Status: %s | FR: %s | RSSI: %d dBm | Free Heap: %u bytes\n",
                    isSystemActive ? "AKTIF" : "STANDBY (SLEEP)",
                    isFrActive ? "SCANNING WAJAH" : "IDLE",
                    rssi, ESP.getFreeHeap());
    }
  }

  delay(30);
}
