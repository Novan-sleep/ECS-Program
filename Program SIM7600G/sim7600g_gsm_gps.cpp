/*
 * sim7600g_gsm_gps.cpp
 * ESP32 DevKit V4 - Modul SIM7600G-H (4G LTE + GPS)
 *
 * Tujuan: inisialisasi modem 4G LTE, cek status jaringan, lalu
 * mengaktifkan dan membaca data GPS (GNSS) dari modul SIM7600G.
 *
 * Hardware:
 *  - ESP32 DevKit V4
 *  - SIM7600G-H via UART1
 *      ESP32 RX1 (GPIO26) <- SIM7600 TXD
 *      ESP32 TX1 (GPIO27) -> SIM7600 RXD
 *      ESP32 GPIO25       -> SIM7600 PWRKEY (tekan sebentar untuk power on)
 *  - Catu daya SIM7600G terpisah (5V/2A min), GND digabung dengan ESP32
 *
 * Pendekatan: AT command manual (tanpa library TinyGSM) agar mudah
 * dikustomisasi dan didebug lewat Serial Monitor.
 */

#include <Arduino.h>

#define MODEM_RX_PIN     26
#define MODEM_TX_PIN     27
#define MODEM_PWRKEY_PIN 25
#define MODEM_BAUD       115200

HardwareSerial ModemSerial(1);

String sendAT(const String &cmd, unsigned long timeout = 2000) {
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

void powerOnModem() {
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(3000); // beri waktu modul boot
}

bool waitModemReady() {
  for (int i = 0; i < 10; i++) {
    String resp = sendAT("AT");
    if (resp.indexOf("OK") >= 0) return true;
    delay(1000);
  }
  return false;
}

void checkNetworkStatus() {
  Serial.println("-- Cek Status Jaringan 4G LTE --");
  Serial.println(sendAT("AT+CPIN?"));      // status SIM card
  Serial.println(sendAT("AT+CSQ"));        // kekuatan sinyal
  Serial.println(sendAT("AT+COPS?"));      // operator terdaftar
  Serial.println(sendAT("AT+CEREG?"));     // status registrasi LTE
  Serial.println(sendAT("AT+CNMP?"));      // mode jaringan aktif
}

void enableGPS() {
  Serial.println("-- Mengaktifkan GPS --");
  Serial.println(sendAT("AT+CGPS=1", 3000)); // enable GNSS
  delay(2000);
}

// Parsing sederhana hasil AT+CGPSINFO menjadi field lat/long/altitude.
bool getGPSLocation(String &raw) {
  String resp = sendAT("AT+CGPSINFO", 3000);
  raw = resp;

  // Format sukses: +CGPSINFO: lat,N/S,lon,E/W,date,time,alt,speed,course
  if (resp.indexOf("+CGPSINFO:") < 0) return false;
  if (resp.indexOf(",,,,") >= 0) return false; // belum ada fix GPS

  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  ModemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  Serial.println();
  Serial.println("=== SIM7600G-H Init (4G LTE + GPS) ===");

  powerOnModem();

  if (!waitModemReady()) {
    Serial.println("Modem tidak merespon. Cek wiring & power supply.");
    return;
  }

  Serial.println("Modem siap.");
  sendAT("ATE0"); // matikan echo
  checkNetworkStatus();
  enableGPS();
}

void loop() {
  String gpsRaw;
  if (getGPSLocation(gpsRaw)) {
    Serial.println("-- Lokasi GPS --");
    Serial.println(gpsRaw);
  } else {
    Serial.println("Menunggu fix GPS...");
  }

  delay(5000);
}
