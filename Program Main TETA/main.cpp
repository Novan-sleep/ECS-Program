/*
 * main.cpp
 * ESP32 DevKit V4 - Program gabungan TETA (satu file):
 *   - SIM7600G-H (4G LTE + GPS)   <- Program SIM7600G/sim7600g_gsm_gps.cpp
 *   - RTC DS3231 + OLED SSD1306   <- RTC dan OLED/rtc_oled.cpp
 *   - Micro SD Card Reader (SPI)  <- SD Card Reader/sd_card_reader.cpp
 *
 * Library yang dibutuhkan (Arduino Library Manager):
 *   - RTClib (Adafruit)
 *   - Adafruit_SSD1306 + Adafruit_GFX
 *   - SD (bawaan ESP32 core), SPI (bawaan ESP32 core)
 *
 * ============================================================
 * PETA PIN (gabungan 3 modul) - sesuai wiring board TETA
 * ============================================================
 *   I2C  (RTC DS3231 + OLED SSD1306)
 *     SDA          -> GPIO 21
 *     SCL          -> GPIO 22
 *   Touch button (navigasi OLED)
 *     TOUCH1 / NEXT -> GPIO 25
 *     TOUCH2 / BACK -> GPIO 33
 *   SPI (SD Card Reader, VSPI default)
 *     SCK          -> GPIO 18
 *     MISO         -> GPIO 19
 *     MOSI         -> GPIO 23
 *     CS           -> GPIO 5
 *   UART1 (SIM7600G-H)
 *     TX_SIM (ESP32 TX) -> GPIO 16  (-> SIM7600 RXD)
 *     RX_SIM (ESP32 RX) -> GPIO 17  (<- SIM7600 TXD)
 *     PWRKEY             -> GPIO 4
 *
 * Lihat "Program SIM7600G/sim7600g_gsm_gps.cpp" untuk catatan lengkap
 * troubleshooting LED NET & daftar AT command SIM7600.
 * ============================================================
 *
 * ============================================================
 * CATATAN BLOCKING & FreeRTOS
 * ============================================================
 * sendAT() menunggu (busy-wait di atas millis()) sampai `timeout` habis
 * tiap kali kirim AT command - bisa 2-15 detik sekali panggil. Kalau
 * dipanggil dari loop() utama yang sama dengan updateDisplayUI(), OLED +
 * tombol touch akan freeze selama itu (paling parah: polling GPS tiap
 * GPS_POLL_INTERVAL_MS menunggu AT+CGPSINFO sampai 3 detik).
 *
 * Solusi: semua komunikasi modem (init, diagnostik, cek internet, polling
 * GPS) dipindah ke task FreeRTOS terpisah (modemTaskFn) yang di-pin ke
 * core 0, sedangkan loop() utama (OLED/touch + logging SD ringan) tetap
 * jalan di core 1 tanpa pernah menunggu modem. Variabel GPS yang dibaca
 * dari kedua task (lastGpsFix/lastGpsRaw) dilindungi mutex (gpsStateMutex)
 * karena tipe String tidak aman diakses bersamaan dari 2 core.
 * ============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ============================================================
// KONFIGURASI PIN
// ============================================================

// ---- I2C : RTC DS3231 + OLED SSD1306 ----
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22

#define OLED_WIDTH       128
#define OLED_HEIGHT      32
#define OLED_ADDR        0x3C

// ---- TOUCH BUTTON (navigasi OLED) ----
#define PIN_TOUCH_NEXT   25   // TOUCH1
#define PIN_TOUCH_BACK   33   // TOUCH2

// ---- SD CARD READER (SPI / VSPI) ----
#define SD_CS_PIN        5
#define SD_SCK_PIN       18
#define SD_MISO_PIN      19
#define SD_MOSI_PIN      23
#define SD_SPI_HZ        4000000
#define LOG_FILE         "/log.txt"

// ---- SIM7600G-H (UART1, 4G LTE + GPS) ----
#define MODEM_TX_PIN     16   // TX_SIM: ESP32 TX -> SIM7600 RXD
#define MODEM_RX_PIN     17   // RX_SIM: ESP32 RX <- SIM7600 TXD
#define MODEM_PWRKEY_PIN 4
#define MODEM_BAUD       115200

// ---- PENJADWALAN NON-BLOCKING (loop) ----
#define GPS_POLL_INTERVAL_MS   5000
#define LOG_INTERVAL_MS        10000

// ============================================================
// OBJEK GLOBAL
// ============================================================

HardwareSerial ModemSerial(1);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
RTC_DS3231 rtc;

static const char *dayNames[] = {"Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"};

#define DEBOUNCE_MS  50
#define TOTAL_PAGE   10

static int currentPage = 0;

// ---- Sensor data (dummy - modul sensor NPK/pH/suhu/kelembaban/baterai
//      belum tersedia di repo ini, nilai dipertahankan sesuai file asli) ----
static float nitrogen   = 42.0;
static float phosphorus = 18.0;
static float potassium  = 35.0;

static float soilPH     = 6.5;
static float soilTemp   = 27.3;
static float soilHumid  = 58.0;

static float batteryVoltage = 11.7;

// volatile: ditulis oleh modemTaskFn (core 0), dibaca oleh loop() UI (core 1)
static volatile bool serverConnected = true;
static volatile bool gpsFix = false;

struct ButtonState {
  bool rawState = false;
  bool stableState = false;
  unsigned long lastDebounce = 0;
  unsigned long pressStart = 0;
  bool longTriggered = false;
};

static ButtonState btnNext;
static ButtonState btnBack;

static volatile bool displayNeedsUpdate = true;

static unsigned long lastLog = 0;

// ---- State GPS dibagi antar task, dilindungi gpsStateMutex ----
// (modemTaskFn menulis di core 0, loop() UI membaca di core 1 untuk logging)
static SemaphoreHandle_t gpsStateMutex = nullptr;
static TaskHandle_t modemTaskHandle = nullptr;
static String lastGpsRaw = "";
static bool lastGpsFix = false;

// ============================================================
// FORWARD DECLARATION
// ============================================================

static void renderHome();
static void nextShort();
static void backShort();
static void updateButton(ButtonState &button, bool rawPressed, unsigned long now,
                          void (*shortPress)(), void (*longPress)());
static bool initSD();
static void writeLog(const String &message);
static void readLog();
static bool initDisplayUI();
static void updateDisplayUI();
static bool initModem();
static void runModemDiagnostics();
static void checkNetworkStatus();
static bool checkInternetConnection(const char *apn = "internet");
static void enableGPS();
static bool getGPSLocation(String &raw);
static String sendAT(const String &cmd, unsigned long timeout = 2000);
static void modemTaskFn(void *pvParameters);

// ============================================================
// ============    MODUL SD CARD READER (SPI)     =============
// Diadaptasi dari "SD Card Reader/sd_card_reader.cpp"
// ============================================================

static bool initSD() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (!SD.begin(SD_CS_PIN, SPI, SD_SPI_HZ)) {
    Serial.println("Inisialisasi SD card gagal. Cek wiring SPI & CS pin.");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("Tidak ada SD card terdeteksi.");
    return false;
  }

  uint64_t cardSizeMB = SD.cardSize() / (1024 * 1024);
  Serial.print("Ukuran SD card: ");
  Serial.print(cardSizeMB);
  Serial.println(" MB");

  return true;
}

static void writeLog(const String &message) {
  File file = SD.open(LOG_FILE, FILE_APPEND);
  if (!file) {
    Serial.println("Gagal membuka file log untuk ditulis.");
    return;
  }
  file.println(message);
  file.close();
}

static void readLog() {
  File file = SD.open(LOG_FILE, FILE_READ);
  if (!file) {
    Serial.println("Gagal membuka file log untuk dibaca.");
    return;
  }

  Serial.println("-- Isi log.txt --");
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
}

// ============================================================
// ========    MODUL RTC DS3231 + OLED SSD1306      ===========
// Diadaptasi dari "RTC dan OLED/rtc_oled.cpp"
// ============================================================

static bool readTouch(int pin) {
  return digitalRead(pin) == HIGH;
}

static void drawHeader(const char* title, int page) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(2, 0);
  display.print(title);

  char pageText[8];
  snprintf(pageText, sizeof(pageText), "%d/%d", page + 1, TOTAL_PAGE);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(pageText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(126 - w, 0);
  display.print(pageText);

  display.drawLine(2, 8, 125, 8, SSD1306_WHITE);
}

static void drawCentered(const char* text, int y, int size) {
  display.setTextSize(size);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);

  int x = (OLED_WIDTH - w) / 2;
  if (x < 0) x = 0;

  display.setCursor(x, y);
  display.print(text);
}

static void drawProgressBar(int x, int y, int width, int height, int percentage) {
  percentage = constrain(percentage, 0, 100);

  display.drawRect(x, y, width, height, SSD1306_WHITE);

  int fillWidth = map(percentage, 0, 100, 0, width - 4);
  if (fillWidth > 0) {
    display.fillRect(x + 2, y + 2, fillWidth, height - 4, SSD1306_WHITE);
  }
}

static void renderNPK(const char* title, float value) {
  display.clearDisplay();
  drawHeader(title, currentPage);

  char valueText[12];
  snprintf(valueText, sizeof(valueText), "%.1f", value);

  display.setTextSize(3);
  display.setCursor(2, 10);
  display.print(valueText);

  display.setTextSize(1);
  display.setCursor(78, 21);
  display.print("mg/kg");

  drawProgressBar(91, 10, 34, 9, constrain((int)value, 0, 100));
}

static void renderPH() {
  display.clearDisplay();
  drawHeader("pH TANAH", currentPage);

  display.setTextSize(3);
  display.setCursor(4, 10);
  display.print(soilPH, 1);

  display.setTextSize(1);
  display.setCursor(61, 13);
  if (soilPH < 5.5) display.print("ASAM");
  else if (soilPH <= 7.0) display.print("NORMAL");
  else display.print("BASA");

  display.drawRect(80, 22, 45, 7, SSD1306_WHITE);

  int pos = map(constrain((int)(soilPH * 10), 0, 140), 0, 140, 0, 40);
  display.fillRect(82 + pos, 24, 3, 3, SSD1306_WHITE);
}

static void renderTemperature() {
  display.clearDisplay();
  drawHeader("SUHU TANAH", currentPage);

  display.setTextSize(3);
  display.setCursor(2, 10);
  display.print(soilTemp, 1);

  display.setTextSize(1);
  display.setCursor(83, 16);
  display.print("\xC2\xB0" "C");

  display.drawRect(108, 11, 5, 12, SSD1306_WHITE);
  display.fillCircle(110, 26, 4, SSD1306_WHITE);
}

static void renderHumidity() {
  display.clearDisplay();
  drawHeader("KELEMBABAN", currentPage);

  display.setTextSize(3);
  display.setCursor(2, 10);
  display.print(soilHumid, 0);
  display.print("%");

  drawProgressBar(86, 10, 39, 9, constrain((int)soilHumid, 0, 100));
}

static void renderBattery() {
  display.clearDisplay();
  drawHeader("BATERAI", currentPage);

  display.setTextSize(3);
  display.setCursor(2, 10);
  display.print(batteryVoltage, 1);

  display.setTextSize(1);
  display.setCursor(78, 17);
  display.print("V");

  int percentage = map(constrain((int)(batteryVoltage * 10), 105, 126), 105, 126, 0, 100);

  display.drawRect(92, 10, 29, 12, SSD1306_WHITE);
  display.fillRect(121, 14, 3, 5, SSD1306_WHITE);

  int fill = map(percentage, 0, 100, 0, 25);
  if (fill > 0) {
    display.fillRect(94, 12, fill, 8, SSD1306_WHITE);
  }

  display.setTextSize(1);
  display.setCursor(99, 24);
  display.print(percentage);
  display.print("%");
}

static void renderServer() {
  display.clearDisplay();
  drawHeader("SERVER", currentPage);

  display.setTextSize(2);
  display.setCursor(2, 11);

  if (serverConnected) {
    display.print("ONLINE");
    display.fillCircle(112, 17, 5, SSD1306_WHITE);
  } else {
    display.print("OFFLINE");
    display.drawCircle(112, 17, 5, SSD1306_WHITE);
    display.drawLine(108, 13, 116, 21, SSD1306_WHITE);
    display.drawLine(116, 13, 108, 21, SSD1306_WHITE);
  }
}

static void renderGPS() {
  display.clearDisplay();
  drawHeader("GPS", currentPage);

  display.setTextSize(2);
  display.setCursor(2, 11);

  if (gpsFix) {
    display.print("FIX");
    display.setTextSize(1);
    display.setCursor(54, 14);
    display.print("READY");

    display.drawCircle(108, 17, 7, SSD1306_WHITE);
    display.fillCircle(108, 17, 2, SSD1306_WHITE);
  } else {
    display.print("SEARCH");
    display.setTextSize(1);
    display.setCursor(82, 15);
    display.print("...");
  }
}

static void renderClock() {
  DateTime now = rtc.now();

  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%s %02d/%02d/%04d",
           dayNames[now.dayOfTheWeek()], now.day(), now.month(), now.year());

  char timeStr[12];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  display.clearDisplay();
  drawHeader("TANGGAL & WAKTU", currentPage);

  display.setTextSize(1);
  display.setCursor(2, 11);
  display.print(dateStr);

  display.setTextSize(2);
  display.setCursor(2, 20);
  display.print(timeStr);
}

static void renderHome() {
  switch (currentPage) {
    case 0: renderNPK("NITROGEN (N)", nitrogen); break;
    case 1: renderNPK("PHOSPHORUS (P)", phosphorus); break;
    case 2: renderNPK("POTASSIUM (K)", potassium); break;
    case 3: renderPH(); break;
    case 4: renderTemperature(); break;
    case 5: renderHumidity(); break;
    case 6: renderBattery(); break;
    case 7: renderServer(); break;
    case 8: renderGPS(); break;
    case 9: renderClock(); break;
  }
}

static void refreshDisplay() {
  if (!displayNeedsUpdate) return;

  renderHome();
  display.display();

  displayNeedsUpdate = false;
}

static void nextShort() {
  currentPage++;
  if (currentPage >= TOTAL_PAGE) currentPage = 0;
  displayNeedsUpdate = true;
}

static void backShort() {
  currentPage--;
  if (currentPage < 0) currentPage = TOTAL_PAGE - 1;
  displayNeedsUpdate = true;
}

static void updateButton(ButtonState &button, bool rawPressed, unsigned long now,
                          void (*shortPress)(), void (*longPress)()) {
  if (rawPressed != button.rawState) {
    button.lastDebounce = now;
    button.rawState = rawPressed;
  }

  if (now - button.lastDebounce >= DEBOUNCE_MS) {
    if (button.stableState != rawPressed) {
      button.stableState = rawPressed;

      if (rawPressed) {
        button.pressStart = now;
      } else if (shortPress != nullptr) {
        shortPress();
      }
    }
  }
}

static unsigned long lastClockTick = 0;

static void updateClockTick() {
  unsigned long now = millis();

  if (now - lastClockTick >= 1000) {
    lastClockTick = now;

    if (currentPage == 9) {
      displayNeedsUpdate = true;
    }
  }
}

// Update status GPS fix yang ditampilkan di halaman GPS.
static void setGpsFixStatus(bool fix) {
  if (fix != gpsFix) {
    gpsFix = fix;
    if (currentPage == 8) displayNeedsUpdate = true;
  }
}

// Update status koneksi server/internet yang ditampilkan di halaman SERVER.
static void setServerStatus(bool connected) {
  if (connected != serverConnected) {
    serverConnected = connected;
    if (currentPage == 7) displayNeedsUpdate = true;
  }
}

static bool initDisplayUI() {
  pinMode(PIN_TOUCH_NEXT, INPUT);
  pinMode(PIN_TOUCH_BACK, INPUT);

  // I2C - dipakai bersama oleh RTC DS3231 dan OLED
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  if (!rtc.begin()) {
    Serial.println("RTC DS3231 tidak terdeteksi. Cek wiring I2C.");
  } else if (rtc.lostPower()) {
    Serial.println("RTC kehilangan daya, set waktu ke waktu compile sketch.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED ERROR! Check OLED wiring/address");
    return false;
  }

  Serial.println("OLED OK!");

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  drawCentered("TETA", 1, 2);
  drawCentered("SOIL SENSOR", 21, 1);
  display.display();

  delay(1800);

  displayNeedsUpdate = true;
  refreshDisplay();

  return true;
}

static void updateDisplayUI() {
  unsigned long now = millis();

  updateButton(btnNext, readTouch(PIN_TOUCH_NEXT), now, nextShort, nullptr);
  updateButton(btnBack, readTouch(PIN_TOUCH_BACK), now, backShort, nullptr);

  updateClockTick();

  refreshDisplay();
}

// ============================================================
// ============    MODUL SIM7600G-H (4G LTE + GPS)     =========
// Diadaptasi dari "Program SIM7600G/sim7600g_gsm_gps.cpp"
// Lihat file asli untuk catatan lengkap troubleshooting LED NET
// & daftar AT command.
// ============================================================

static String sendAT(const String &cmd, unsigned long timeout) {
  ModemSerial.println(cmd);
  String response = "";
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (ModemSerial.available()) {
      response += (char)ModemSerial.read();
    }
  }
  response.trim();
  return response;
}

static void powerOnModem() {
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(3000); // beri waktu modul boot
}

static bool waitModemReady() {
  for (int i = 0; i < 10; i++) {
    String resp = sendAT("AT");
    if (resp.indexOf("OK") >= 0) return true;
    delay(1000);
  }
  return false;
}

static bool initModem() {
  ModemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  powerOnModem();

  if (!waitModemReady()) {
    Serial.println("Modem tidak merespon. Cek wiring & power supply.");
    return false;
  }

  Serial.println("Modem siap.");
  sendAT("ATE0"); // matikan echo
  return true;
}

static void checkNetworkStatus() {
  Serial.println("-- Cek Status Jaringan 4G LTE --");
  Serial.println(sendAT("AT+CPIN?"));      // status SIM card
  Serial.println(sendAT("AT+CSQ"));        // kekuatan sinyal
  Serial.println(sendAT("AT+COPS?"));      // operator terdaftar
  Serial.println(sendAT("AT+CEREG?"));     // status registrasi LTE
  Serial.println(sendAT("AT+CNMP?"));      // mode jaringan aktif
}

static void runModemDiagnostics() {
  Serial.println();
  Serial.println("======== DIAGNOSTIK SIM7600G-H ========");

  Serial.println("[1] Cek modul merespon AT ...");
  Serial.println(sendAT("AT"));

  Serial.println("[2] Info modul (model & firmware) ...");
  Serial.println(sendAT("ATI"));
  Serial.println(sendAT("AT+CGMR"));

  Serial.println("[3] Cek SIM card ...");
  String cpin = sendAT("AT+CPIN?");
  Serial.println(cpin);
  if (cpin.indexOf("READY") >= 0) {
    Serial.println("    -> SIM terbaca & tidak terkunci. OK.");
  } else if (cpin.indexOf("SIM PIN") >= 0) {
    Serial.println("    -> SIM terkunci PIN! Kirim AT+CPIN=\"kode_pin\" dulu.");
  } else {
    Serial.println("    -> SIM TIDAK TERBACA. Cek pemasangan fisik SIM card.");
  }
  Serial.println(sendAT("AT+CCID"));

  Serial.println("[4] Kekuatan sinyal (RSSI) ...");
  Serial.println(sendAT("AT+CSQ"));

  Serial.println("[5] Status registrasi jaringan ...");
  Serial.println(sendAT("AT+CREG?"));
  Serial.println(sendAT("AT+CEREG?"));

  Serial.println("[6] Operator & mode jaringan ...");
  Serial.println(sendAT("AT+COPS?"));
  Serial.println(sendAT("AT+CNMP?"));

  Serial.println("[7] Catu daya ...");
  Serial.println(sendAT("AT+CBC"));

  Serial.println("========================================");
  Serial.println();
}

// Terjemahkan <statuscode> +HTTPACTION (manual SIMCOM HTTP AT Command bab 4 & 5).
static String interpretHttpStatus(int code) {
  switch (code) {
    case 200: return "OK - request berhasil, internet AKTIF & terpakai";
    case 301: case 302: case 303: case 307:
      return "Redirect. Koneksi internet sebenarnya JALAN.";
    case 400: return "Bad Request (URL/parameter salah dari sisi kita)";
    case 403: return "Forbidden (server menolak akses)";
    case 404: return "Not Found (URL tidak ada di server, tapi internet JALAN)";
    case 500: case 502: case 503: case 504:
      return "Error di sisi server tujuan (bukan masalah modul/internet kita)";
    case 600: return "Not HTTP PDU (respons bukan format HTTP valid)";
    case 601: return "Network Error - masalah jaringan data";
    case 602: return "No memory di modul";
    case 603: return "DNS Error - gagal resolve domain";
    case 604: return "Stack Busy - coba ulang beberapa saat lagi";
    case 0:   return "Success (kode 0 dari tabel error internal)";
    case 701: return "Alert state";
    case 702: return "Unknown error - biasanya PDP context/APN belum siap atau sinyal lemah";
    case 703: return "Busy - sesi HTTP sebelumnya belum ditutup";
    case 704: return "Connection closed error";
    case 705: return "Timeout";
    case 706: return "Gagal kirim/terima data socket";
    case 707: return "File tidak ada / error memori lain";
    case 708: return "Parameter tidak valid";
    case 709: return "Network error (level socket)";
    case 712: return "Gagal membuat socket";
    case 713: return "Get DNS failed";
    case 714: return "Gagal connect socket ke server";
    case 715: return "Handshake gagal (khusus HTTPS/SSL)";
    case 717: return "No network error - tidak ada jaringan data sama sekali";
    case 718: return "Send data timeout";
    default:  return "Kode tidak dikenal, cek manual SIMCOM HTTP AT Command bab 4 & 5";
  }
}

static int doHttpGet(const char *url) {
  sendAT("AT+HTTPTERM");
  Serial.println(sendAT("AT+HTTPINIT", 10000));
  Serial.println(sendAT("AT+HTTPPARA=\"CID\",1"));
  String setUrl = "AT+HTTPPARA=\"URL\",\"" + String(url) + "\"";
  Serial.println(sendAT(setUrl));

  String httpAction = sendAT("AT+HTTPACTION=0", 15000);
  Serial.println(httpAction);
  String httpUrc = httpAction;
  if (httpUrc.indexOf("+HTTPACTION:") < 0) {
    delay(3000);
    httpUrc = sendAT("", 3000);
    Serial.println(httpUrc);
  }
  sendAT("AT+HTTPTERM");

  int actionPos = httpUrc.indexOf("+HTTPACTION:");
  if (actionPos < 0) return -1;
  int firstComma = httpUrc.indexOf(',', actionPos);
  int secondComma = httpUrc.indexOf(',', firstComma + 1);
  if (firstComma < 0 || secondComma < 0) return -1;
  return httpUrc.substring(firstComma + 1, secondComma).toInt();
}

static bool checkInternetConnection(const char *apn) {
  Serial.println();
  Serial.println("-- Cek Koneksi Internet (Data/PDP Context) --");

  String setApn = "AT+CGDCONT=1,\"IP\",\"" + String(apn) + "\"";
  Serial.println(sendAT(setApn));

  String ipResp = sendAT("AT+CGPADDR=1");
  Serial.println(ipResp);
  int ipPrefixPos = ipResp.indexOf("+CGPADDR: 1,");
  bool hasIP = false;
  if (ipPrefixPos >= 0) {
    int ipStart = ipPrefixPos + strlen("+CGPADDR: 1,");
    if (ipStart < (int)ipResp.length()) {
      char c = ipResp.charAt(ipStart);
      hasIP = (c != ',' && c != '\r' && c != '\n');
      if (ipResp.substring(ipStart).startsWith("0.0.0.0")) hasIP = false;
    }
  }

  if (!hasIP) {
    Serial.println("    -> BELUM dapat IP address. PDP context gagal aktif.");
    return false;
  }
  Serial.println("    -> Dapat IP address. Modul terhubung ke jaringan data operator.");

  Serial.println("-- Tes HTTP GET ke http://neverssl.com --");
  int statusCode = doHttpGet("http://neverssl.com");
  bool httpOk = (statusCode == 200);
  if (statusCode >= 0) {
    Serial.print("    -> Status code: "); Serial.print(statusCode);
    Serial.print(" => "); Serial.println(interpretHttpStatus(statusCode));
  } else {
    Serial.println("    -> Tidak menangkap respons +HTTPACTION sama sekali (timeout total).");
  }

  if (!httpOk) {
    Serial.println("-- Tes 2: HTTP GET ke http://1.1.1.1 (IP langsung) --");
    int statusCode2 = doHttpGet("http://1.1.1.1");
    if (statusCode2 >= 0) {
      Serial.print("    -> Status code: "); Serial.print(statusCode2);
      Serial.print(" => "); Serial.println(interpretHttpStatus(statusCode2));
    }
    if (statusCode2 == 200 || statusCode2 == 400) {
      httpOk = true;
    }
  }

  Serial.println(httpOk ? "    -> INTERNET AKTIF & BISA DIPAKAI." : "    -> Belum terkonfirmasi tembus internet.");
  return httpOk;
}

static void enableGPS() {
  Serial.println("-- Mengaktifkan GPS --");
  Serial.println(sendAT("AT+CGPS=1", 3000));
  delay(2000);
}

static bool getGPSLocation(String &raw) {
  String resp = sendAT("AT+CGPSINFO", 3000);
  raw = resp;

  if (resp.indexOf("+CGPSINFO:") < 0) return false;
  if (resp.indexOf(",,,,") >= 0) return false; // belum ada fix GPS

  return true;
}

// ============================================================
// MODEM TASK (FreeRTOS, dijalankan di core 0)
// Semua sendAT() blocking (2-15 detik) terjadi di sini, terpisah dari
// task UI/loop() di core 1 supaya OLED + tombol touch tidak freeze.
// ============================================================

static void modemTaskFn(void *pvParameters) {
  Serial.println();
  Serial.println("=== SIM7600G-H Init (4G LTE + GPS) ===");

  bool modemReady = initModem();
  if (modemReady) {
    runModemDiagnostics();  // troubleshooting LED NET kedip cepat, lihat file asli
    checkNetworkStatus();
    bool internetOk = checkInternetConnection("internet"); // ganti APN sesuai SIM
    setServerStatus(internetOk);
    enableGPS();
  } else {
    setServerStatus(false);
  }

  for (;;) {
    String gpsRaw;
    bool fix = getGPSLocation(gpsRaw);

    if (xSemaphoreTake(gpsStateMutex, portMAX_DELAY) == pdTRUE) {
      lastGpsFix = fix;
      lastGpsRaw = gpsRaw;
      xSemaphoreGive(gpsStateMutex);
    }

    setGpsFixStatus(fix);

    if (fix) {
      Serial.println("-- Lokasi GPS --");
      Serial.println(gpsRaw);
    } else {
      Serial.println("Menunggu fix GPS...");
    }

    vTaskDelay(pdMS_TO_TICKS(GPS_POLL_INTERVAL_MS));
  }
}

// ============================================================
// ====================    SETUP / LOOP    =====================
// ============================================================

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println();
  Serial.println("=== Program Main TETA ===");

  // ---- SD Card ----
  Serial.println();
  Serial.println("=== SD Card Reader (SPI) ===");
  bool sdReady = initSD();
  if (sdReady) {
    writeLog("Program Main TETA start, millis=" + String(millis()));
  }

  // ---- RTC + OLED ----
  Serial.println();
  Serial.println("=== RTC DS3231 + OLED SSD1306 ===");
  initDisplayUI();

  // ---- Modem SIM7600G (4G LTE + GPS) ----
  // Jalan di task terpisah (core 0) karena AT command blocking sampai 15
  // detik - kalau dipanggil langsung di loop() utama, OLED + touch freeze.
  gpsStateMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(
    modemTaskFn,
    "ModemTask",
    8192,       // stack size - AT command pakai String, butuh cukup besar
    nullptr,
    1,          // priority
    &modemTaskHandle,
    0           // core 0 (loop() UI utama otomatis jalan di core 1)
  );
}

void loop() {
  unsigned long now = millis();

  // UI OLED + navigasi touch - harus dipanggil tiap iterasi supaya responsif.
  // Tidak lagi menunggu modem karena polling GPS sudah pindah ke modemTaskFn.
  updateDisplayUI();

  // Tulis log ke SD card secara berkala (non-blocking, hanya baca state GPS
  // terakhir yang di-share lewat mutex, tidak pernah menunggu modem).
  if (now - lastLog >= LOG_INTERVAL_MS) {
    lastLog = now;

    bool fix = false;
    String gpsRaw;
    if (xSemaphoreTake(gpsStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      fix = lastGpsFix;
      gpsRaw = lastGpsRaw;
      xSemaphoreGive(gpsStateMutex);
    }

    DateTime ts = rtc.now();
    char tsStr[24];
    snprintf(tsStr, sizeof(tsStr), "%04d-%02d-%02d %02d:%02d:%02d",
             ts.year(), ts.month(), ts.day(), ts.hour(), ts.minute(), ts.second());

    String line = String(tsStr) + " | GPS_FIX=" + (fix ? "1" : "0");
    if (fix) {
      line += " | " + gpsRaw;
    }

    writeLog(line);
  }

  // Beri jatah waktu ke task lain (idle task/watchdog) - loop ini sendiri
  // tidak pernah blocking lama, jadi delay singkat saja cukup.
  vTaskDelay(pdMS_TO_TICKS(10));
}
