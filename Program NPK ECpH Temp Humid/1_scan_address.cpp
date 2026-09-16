/*
 * 1_scan_address.cpp
 * ESP32 DevKit V4 - Modbus RTU Address Scanner
 *
 * Tujuan: memindai bus RS485 untuk menemukan slave address yang aktif.
 * Dipakai untuk mendeteksi konflik address antara sensor NPK DFRobot
 * dan sensor EC/pH/Temp/Humid DFRobot yang secara default punya
 * slave address sama (umumnya 0x01, baud 4800).
 *
 * Hardware:
 *  - ESP32 DevKit V4
 *  - Modul RS485 (MAX485 / MAX3485) ke UART2
 *      ESP32 RX2 (GPIO16) <- RO
 *      ESP32 TX2 (GPIO17) -> DI
 *      ESP32 GPIO4        -> DE & RE (digabung, kontrol arah)
 *
 * Library: ModbusMaster (https://github.com/4-20ma/ModbusMaster)
 */

#include <Arduino.h>
#include <ModbusMaster.h>

#define RS485_DE_RE_PIN 4
#define MODBUS_BAUD      9600   // Default baud rate SEN0604/SEN0605: 9600

HardwareSerial RS485Serial(2); // UART2
ModbusMaster node;

void preTransmission() {
  digitalWrite(RS485_DE_RE_PIN, HIGH);
}

void postTransmission() {
  digitalWrite(RS485_DE_RE_PIN, LOW);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);

  RS485Serial.begin(MODBUS_BAUD, SERIAL_8N1, 16, 17);

  Serial.println();
  Serial.println("=== Modbus RTU Address Scanner ===");
  Serial.println("Memindai slave address 1 - 247 ...");
}

void loop() {
  for (uint8_t addr = 1; addr <= 247; addr++) {
    node.begin(addr, RS485Serial);
    node.preTransmission(preTransmission);
    node.postTransmission(postTransmission);

    // Coba baca 1 holding register pertama (register 0x0000) sebagai probe.
    uint8_t result = node.readHoldingRegisters(0x0000, 1);

    if (result == node.ku8MBSuccess) {
      Serial.print("Ditemukan device pada address: ");
      Serial.print(addr);
      Serial.print(" (0x");
      Serial.print(addr, HEX);
      Serial.println(")");
    }

    delay(50); // beri jeda antar probe agar bus tidak flood
  }

  Serial.println("--- Scan selesai. Mengulang dalam 10 detik ---");
  delay(10000);
}
