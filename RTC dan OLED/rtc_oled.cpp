/*
 * rtc_oled.cpp
 * ESP32 DevKit V4 - RTC DS3231 + OLED SSD1306 0.91" (128x32), I2C
 * UI multi-halaman dengan navigasi touch (NEXT/BACK)
 *
 * Hardware:
 *  - ESP32 DevKit V4
 *  - DS3231 dan SSD1306 berbagi 1 bus I2C
 *      ESP32 SDA (GPIO21) -> SDA (RTC & OLED)
 *      ESP32 SCL (GPIO22) -> SCL (RTC & OLED)
 *  - DS3231 default address: 0x68
 *  - SSD1306 0.91" default address: 0x3C
 *  - Touch 1 / NEXT -> GPIO 4 (VCC 3.3V, GND, short press = next)
 *  - Touch 2 / BACK -> GPIO 5 (VCC 3.3V, GND, short press = back)
 *
 * Library:
 *  - RTClib (Adafruit)
 *  - Adafruit_SSD1306 + Adafruit_GFX
 */

#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =====================================================
// OLED
// =====================================================

#define OLED_WIDTH   128
#define OLED_HEIGHT  32

#define OLED_SDA     21
#define OLED_SCL     22

#define OLED_ADDR    0x3C

Adafruit_SSD1306 display(
  OLED_WIDTH,
  OLED_HEIGHT,
  &Wire,
  -1
);

// =====================================================
// RTC (DS3231) - berbagi bus I2C dengan OLED
// =====================================================

RTC_DS3231 rtc;

const char *dayNames[] = {"Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"};

// =====================================================
// TOUCH (2 MODULES)
// =====================================================
//
// TOUCH 1 / NEXT
//   OUT -> GPIO 4
//   VCC -> 3.3V
//   GND -> GND
//   Short press -> NEXT
//
// TOUCH 2 / BACK
//   OUT -> GPIO 5
//   VCC -> 3.3V
//   GND -> GND
//   Short press -> BACK
// =====================================================

#define PIN_TOUCH_NEXT  4
#define PIN_TOUCH_BACK  5

// =====================================================
// TIMING
// =====================================================

#define DEBOUNCE_MS    50

// =====================================================
// PAGE
// =====================================================

#define TOTAL_PAGE 10

int currentPage = 0;

// =====================================================
// SENSOR DATA - DUMMY
// =====================================================

float nitrogen   = 42.0;
float phosphorus = 18.0;
float potassium  = 35.0;

float soilPH     = 6.5;
float soilTemp   = 27.3;
float soilHumid  = 58.0;

float batteryVoltage = 11.7;

bool serverConnected = true;
bool gpsFix = true;

// =====================================================
// BUTTON
// =====================================================

struct ButtonState {
  bool rawState = false;
  bool stableState = false;

  unsigned long lastDebounce = 0;
  unsigned long pressStart = 0;

  bool longTriggered = false;
};

ButtonState btnNext;
ButtonState btnBack;

// =====================================================
// DISPLAY CONTROL
// =====================================================

bool displayNeedsUpdate = true;

// =====================================================
// FUNCTION DECLARATION
// =====================================================

void renderHome();

void nextShort();
void backShort();

void updateButton(
  ButtonState &button,
  bool rawPressed,
  unsigned long now,
  void (*shortPress)(),
  void (*longPress)()
);

// =====================================================
// TOUCH READ
// =====================================================

bool readTouch(int pin) {
  return digitalRead(pin) == HIGH;
}

// =====================================================
// DRAW HEADER
// =====================================================

void drawHeader(const char* title, int page) {

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Title
  display.setCursor(2, 0);
  display.print(title);

  // Page number
  char pageText[8];

  snprintf(
    pageText,
    sizeof(pageText),
    "%d/%d",
    page + 1,
    TOTAL_PAGE
  );

  int16_t x1, y1;
  uint16_t w, h;

  display.getTextBounds(
    pageText,
    0,
    0,
    &x1,
    &y1,
    &w,
    &h
  );

  display.setCursor(
    126 - w,
    0
  );

  display.print(pageText);

  // Header separator
  display.drawLine(
    2,
    8,
    125,
    8,
    SSD1306_WHITE
  );
}

// =====================================================
// CENTER TEXT
// =====================================================

void drawCentered(
  const char* text,
  int y,
  int size
) {

  display.setTextSize(size);

  int16_t x1, y1;
  uint16_t w, h;

  display.getTextBounds(
    text,
    0,
    y,
    &x1,
    &y1,
    &w,
    &h
  );

  int x = (OLED_WIDTH - w) / 2;

  if (x < 0) {
    x = 0;
  }

  display.setCursor(x, y);
  display.print(text);
}

// =====================================================
// PROGRESS BAR
// =====================================================

void drawProgressBar(
  int x,
  int y,
  int width,
  int height,
  int percentage
) {

  percentage = constrain(
    percentage,
    0,
    100
  );

  display.drawRect(
    x,
    y,
    width,
    height,
    SSD1306_WHITE
  );

  int fillWidth = map(
    percentage,
    0,
    100,
    0,
    width - 4
  );

  if (fillWidth > 0) {

    display.fillRect(
      x + 2,
      y + 2,
      fillWidth,
      height - 4,
      SSD1306_WHITE
    );
  }
}

// =====================================================
// NPK PAGE
// =====================================================

void renderNPK(
  const char* title,
  float value
) {

  display.clearDisplay();

  drawHeader(
    title,
    currentPage
  );

  // Main number - enlarged
  char valueText[12];

  snprintf(
    valueText,
    sizeof(valueText),
    "%.1f",
    value
  );

  display.setTextSize(3);
  display.setCursor(2, 10);
  display.print(valueText);

  // Unit
  display.setTextSize(1);
  display.setCursor(78, 21);
  display.print("mg/kg");

  // Small indicator bar at the right
  drawProgressBar(
    91,
    10,
    34,
    9,
    constrain((int)value, 0, 100)
  );
}

// =====================================================
// pH
// =====================================================

void renderPH() {

  display.clearDisplay();

  drawHeader(
    "pH TANAH",
    currentPage
  );

  // Large value
  display.setTextSize(3);
  display.setCursor(4, 10);
  display.print(soilPH, 1);

  // Status
  display.setTextSize(1);
  display.setCursor(61, 13);

  if (soilPH < 5.5) {
    display.print("ASAM");
  }
  else if (soilPH <= 7.0) {
    display.print("NORMAL");
  }
  else {
    display.print("BASA");
  }

  // pH scale
  display.drawRect(
    80,
    22,
    45,
    7,
    SSD1306_WHITE
  );

  int pos = map(
    constrain((int)(soilPH * 10), 0, 140),
    0,
    140,
    0,
    40
  );

  display.fillRect(
    82 + pos,
    24,
    3,
    3,
    SSD1306_WHITE
  );
}

// =====================================================
// TEMPERATURE
// =====================================================

void renderTemperature() {

  display.clearDisplay();

  drawHeader(
    "SUHU TANAH",
    currentPage
  );

  display.setTextSize(3);
  display.setCursor(2, 10);
  display.print(soilTemp, 1);

  display.setTextSize(1);
  display.setCursor(83, 16);
  display.print("°C");

  // Thermometer icon
  display.drawRect(
    108,
    11,
    5,
    12,
    SSD1306_WHITE
  );

  display.fillCircle(
    110,
    26,
    4,
    SSD1306_WHITE
  );
}

// =====================================================
// HUMIDITY
// =====================================================

void renderHumidity() {

  display.clearDisplay();

  drawHeader(
    "KELEMBABAN",
    currentPage
  );

  // Large percentage
  display.setTextSize(3);
  display.setCursor(2, 10);
  display.print(soilHumid, 0);
  display.print("%");

  // Bar
  drawProgressBar(
    86,
    10,
    39,
    9,
    constrain((int)soilHumid, 0, 100)
  );
}

// =====================================================
// BATTERY
// =====================================================

void renderBattery() {

  display.clearDisplay();

  drawHeader(
    "BATERAI",
    currentPage
  );

  // Large voltage
  display.setTextSize(3);
  display.setCursor(2, 10);
  display.print(batteryVoltage, 1);

  display.setTextSize(1);
  display.setCursor(78, 17);
  display.print("V");

  // Battery percentage
  int percentage = map(
    constrain(
      (int)(batteryVoltage * 10),
      105,
      126
    ),
    105,
    126,
    0,
    100
  );

  // Battery icon
  display.drawRect(
    92,
    10,
    29,
    12,
    SSD1306_WHITE
  );

  display.fillRect(
    121,
    14,
    3,
    5,
    SSD1306_WHITE
  );

  int fill = map(
    percentage,
    0,
    100,
    0,
    25
  );

  if (fill > 0) {

    display.fillRect(
      94,
      12,
      fill,
      8,
      SSD1306_WHITE
    );
  }

  display.setTextSize(1);
  display.setCursor(
    99,
    24
  );
  display.print(percentage);
  display.print("%");
}

// =====================================================
// SERVER
// =====================================================

void renderServer() {

  display.clearDisplay();

  drawHeader(
    "SERVER",
    currentPage
  );

  // Status text uses size 2 so ONLINE/OFFLINE always fits
  display.setTextSize(2);
  display.setCursor(2, 11);

  if (serverConnected) {

    display.print("ONLINE");

    display.fillCircle(
      112,
      17,
      5,
      SSD1306_WHITE
    );

  }
  else {

    display.print("OFFLINE");

    display.drawCircle(
      112,
      17,
      5,
      SSD1306_WHITE
    );

    display.drawLine(
      108,
      13,
      116,
      21,
      SSD1306_WHITE
    );

    display.drawLine(
      116,
      13,
      108,
      21,
      SSD1306_WHITE
    );
  }
}

// =====================================================
// GPS
// =====================================================

void renderGPS() {

  display.clearDisplay();

  drawHeader(
    "GPS",
    currentPage
  );

  // Status text uses size 2 so SEARCH always fits
  display.setTextSize(2);
  display.setCursor(2, 11);

  if (gpsFix) {

    display.print("FIX");

    display.setTextSize(1);
    display.setCursor(54, 14);
    display.print("READY");

    // GPS icon
    display.drawCircle(
      108,
      17,
      7,
      SSD1306_WHITE
    );

    display.fillCircle(
      108,
      17,
      2,
      SSD1306_WHITE
    );

  }
  else {

    display.print("SEARCH");

    display.setTextSize(1);
    display.setCursor(82, 15);
    display.print("...");
  }
}

// =====================================================
// TANGGAL & WAKTU (RTC DS3231)
// =====================================================

void renderClock() {

  DateTime now = rtc.now();

  char dateStr[16];
  snprintf(
    dateStr,
    sizeof(dateStr),
    "%s %02d/%02d/%04d",
    dayNames[now.dayOfTheWeek()],
    now.day(),
    now.month(),
    now.year()
  );

  char timeStr[12];
  snprintf(
    timeStr,
    sizeof(timeStr),
    "%02d:%02d:%02d",
    now.hour(),
    now.minute(),
    now.second()
  );

  display.clearDisplay();

  drawHeader(
    "TANGGAL & WAKTU",
    currentPage
  );

  display.setTextSize(1);
  display.setCursor(2, 11);
  display.print(dateStr);

  display.setTextSize(2);
  display.setCursor(2, 20);
  display.print(timeStr);
}

// =====================================================
// HOME
// =====================================================

void renderHome() {

  switch (currentPage) {

    case 0:
      renderNPK(
        "NITROGEN (N)",
        nitrogen
      );
      break;

    case 1:
      renderNPK(
        "PHOSPHORUS (P)",
        phosphorus
      );
      break;

    case 2:
      renderNPK(
        "POTASSIUM (K)",
        potassium
      );
      break;

    case 3:
      renderPH();
      break;

    case 4:
      renderTemperature();
      break;

    case 5:
      renderHumidity();
      break;

    case 6:
      renderBattery();
      break;

    case 7:
      renderServer();
      break;

    case 8:
      renderGPS();
      break;

    case 9:
      renderClock();
      break;
  }
}

// =====================================================
// DISPLAY UPDATE
// =====================================================

void updateDisplay() {

  if (!displayNeedsUpdate) {
    return;
  }

  renderHome();

  display.display();

  displayNeedsUpdate = false;
}


// =====================================================
// TOUCH 1 : NEXT
// SHORT PRESS = NEXT
// =====================================================

void nextShort() {

  currentPage++;

  if (currentPage >= TOTAL_PAGE) {
    currentPage = 0;
  }

  displayNeedsUpdate = true;
}


// =====================================================
// TOUCH 2 : BACK
// SHORT PRESS = BACK
// =====================================================

void backShort() {

  currentPage--;

  if (currentPage < 0) {
    currentPage = TOTAL_PAGE - 1;
  }

  displayNeedsUpdate = true;
}


// =====================================================
// BUTTON PROCESSING
// =====================================================

void updateButton(
  ButtonState &button,
  bool rawPressed,
  unsigned long now,
  void (*shortPress)(),
  void (*longPress)()
) {

  if (rawPressed != button.rawState) {
    button.lastDebounce = now;
    button.rawState = rawPressed;
  }

  if (now - button.lastDebounce >= DEBOUNCE_MS) {

    if (button.stableState != rawPressed) {

      button.stableState = rawPressed;

      if (rawPressed) {
        button.pressStart = now;
      }
      else {
        if (shortPress != nullptr) {
          shortPress();
        }
      }
    }
  }
}

// =====================================================
// PAGE AUTO-REFRESH (halaman yang datanya berubah tiap detik)
// =====================================================

unsigned long lastClockTick = 0;

void updateClockTick() {

  unsigned long now = millis();

  if (now - lastClockTick >= 1000) {

    lastClockTick = now;

    if (currentPage == 9) {
      displayNeedsUpdate = true;
    }
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(300);

  // Touch pins
  pinMode(
    PIN_TOUCH_NEXT,
    INPUT
  );

  pinMode(
    PIN_TOUCH_BACK,
    INPUT
  );

  // I2C - dipakai bersama oleh RTC DS3231 dan OLED
  Wire.begin(
    OLED_SDA,
    OLED_SCL
  );

  Wire.setClock(400000);

  // RTC DS3231
  if (!rtc.begin()) {

    Serial.println(
      "RTC DS3231 tidak terdeteksi. Cek wiring I2C."
    );
  }
  else if (rtc.lostPower()) {

    Serial.println(
      "RTC kehilangan daya, set waktu ke waktu compile sketch."
    );

    rtc.adjust(
      DateTime(F(__DATE__), F(__TIME__))
    );
  }

  // OLED
  if (
    !display.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDR
    )
  ) {

    Serial.println(
      "OLED ERROR!"
    );

    while (true) {

      delay(1000);

      Serial.println(
        "Check OLED wiring/address"
      );
    }
  }

  Serial.println(
    "OLED OK!"
  );

  display.clearDisplay();

  display.setTextColor(
    SSD1306_WHITE
  );

  // Startup
  drawCentered(
    "TETA",
    1,
    2
  );

  drawCentered(
    "SOIL SENSOR",
    21,
    1
  );

  display.display();

  delay(1800);

  displayNeedsUpdate = true;

  updateDisplay();
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  unsigned long now = millis();

  // Touch 1 = NEXT
  updateButton(
    btnNext,
    readTouch(PIN_TOUCH_NEXT),
    now,
    nextShort,
    nullptr
  );

  // Touch 2 = BACK
  updateButton(
    btnBack,
    readTouch(PIN_TOUCH_BACK),
    now,
    backShort,
    nullptr
  );

  // Refresh halaman jam tiap detik saat sedang ditampilkan
  updateClockTick();

  updateDisplay();

  delay(10);
}
