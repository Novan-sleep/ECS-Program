/*
 * rtc_oled.cpp
 * ESP32 DevKit V4 - RTC DS3231 + OLED SSD1306 0.91" (128x32), I2C
 *
 * Tujuan: membaca waktu dari modul RTC DS3231 dan menampilkannya
 * di layar OLED SSD1306 0.91 inch (resolusi 128x32).
 *
 * Hardware:
 *  - ESP32 DevKit V4
 *  - DS3231 dan SSD1306 berbagi 1 bus I2C
 *      ESP32 SDA (GPIO21) -> SDA (RTC & OLED)
 *      ESP32 SCL (GPIO22) -> SCL (RTC & OLED)
 *  - DS3231 default address: 0x68
 *  - SSD1306 0.91" default address: 0x3C
 *
 * Library:
 *  - RTClib (Adafruit)
 *  - Adafruit_SSD1306 + Adafruit_GFX
 */

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 32
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
RTC_DS3231 rtc;

const char *dayNames[] = {"Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"};

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Wire.begin(21, 22);

  Serial.println();
  Serial.println("=== RTC DS3231 + OLED SSD1306 0.91\" ===");

  if (!rtc.begin()) {
    Serial.println("RTC DS3231 tidak terdeteksi. Cek wiring I2C.");
    while (1) delay(1000);
  }

  if (rtc.lostPower()) {
    Serial.println("RTC kehilangan daya, set waktu ke waktu compile sketch.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED SSD1306 tidak terdeteksi. Cek wiring I2C / address.");
    while (1) delay(1000);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
}

void loop() {
  DateTime now = rtc.now();

  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d", now.day(), now.month(), now.year());

  char timeStr[16];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  Serial.print(dayNames[now.dayOfTheWeek()]);
  Serial.print(" ");
  Serial.print(dateStr);
  Serial.print(" ");
  Serial.println(timeStr);

  display.clearDisplay();

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(dayNames[now.dayOfTheWeek()]);
  display.print(" ");
  display.println(dateStr);

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(timeStr);

  display.display();

  delay(1000);
}
