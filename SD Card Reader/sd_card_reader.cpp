/*
 * sd_card_reader.cpp
 * ESP32 DevKit V4 - Micro SD Card Reader Module (SPI)
 *
 * Tujuan: inisialisasi modul micro SD card reader dan menyediakan
 * fungsi dasar tulis (log) & baca file.
 *
 * Hardware:
 *  - ESP32 DevKit V4
 *  - Modul SD Card Reader via VSPI (default ESP32)
 *      ESP32 GPIO18 (SCK)  -> SCK
 *      ESP32 GPIO19 (MISO) -> MISO
 *      ESP32 GPIO23 (MOSI) -> MOSI
 *      ESP32 GPIO5  (CS)   -> CS
 *
 * Library: SD.h + SPI.h (built-in Arduino core untuk ESP32)
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#define SD_CS_PIN   5
#define SD_SCK_PIN  18
#define SD_MISO_PIN 19
#define SD_MOSI_PIN 23
#define SD_SPI_HZ   4000000
#define LOG_FILE    "/log.txt"

void writeLog(const String &message) {
  File file = SD.open(LOG_FILE, FILE_APPEND);
  if (!file) {
    Serial.println("Gagal membuka file log untuk ditulis.");
    return;
  }
  file.println(message);
  file.close();
}

void readLog() {
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

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println();
  Serial.println("=== SD Card Reader (SPI) ===");

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (!SD.begin(SD_CS_PIN, SPI, SD_SPI_HZ)) {
    Serial.println("Inisialisasi SD card gagal. Cek wiring SPI & CS pin.");
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("Tidak ada SD card terdeteksi.");
    return;
  }

  uint64_t cardSizeMB = SD.cardSize() / (1024 * 1024);
  Serial.print("Ukuran SD card: ");
  Serial.print(cardSizeMB);
  Serial.println(" MB");

  writeLog("SD card init OK, millis=" + String(millis()));
  readLog();
}

void loop() {
  // Contoh logging berkala setiap 5 detik.
  writeLog("Log entry, millis=" + String(millis()));
  delay(5000);
}
