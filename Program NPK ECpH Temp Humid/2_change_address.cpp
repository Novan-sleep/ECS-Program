/*
 * 2_change_address.cpp
 * ESP32 DevKit V4 - Ubah Slave Address Sensor DFRobot EC/pH/Temp/Humid (SEN0604)
 *
 * Tujuan: sensor NPK (SEN0605) dan sensor EC/pH/Temp/Humid (SEN0604) DFRobot
 * punya default slave address sama (0x01, baud 9600). Program ini mengubah
 * address SALAH SATU sensor saja (SEN0604) ke address baru agar keduanya
 * bisa berbagi 1 bus RS485.
 *
 * Register map resmi (DFRobot Wiki SEN0604):
 *   https://wiki.dfrobot.com/sen0604/docs/20297
 *   0x07D0 - Device Address (range 1-254, default 1), fungsi 0x06 (write single register)
 *   0x07D1 - Baud Rate (0=2400, 1=4800, 2=9600), default 2 (9600)
 *
 * PENTING:
 *  - Sambungkan HANYA sensor EC/pH/Temp/Humid (SEN0604) ke bus saat
 *    menjalankan program ini (lepas sensor NPK/SEN0605 dulu), supaya tidak
 *    salah ubah address sensor yang salah karena keduanya masih di address
 *    default yang sama (0x01).
 *  - Setelah address berubah, sensor perlu di-power cycle (matikan-nyalakan)
 *    agar perubahan berlaku penuh.
 *
 * Hardware: sama seperti 1_scan_address.cpp
 */

#include <Arduino.h>
#include <ModbusMaster.h>

#define RS485_DE_RE_PIN 4
#define MODBUS_BAUD      9600   // Default baud rate SEN0604: 9600

#define CURRENT_ADDRESS     0x01   // Address default pabrik SEN0604
#define NEW_ADDRESS         0x02   // Address baru yang diinginkan (hindari bentrok dgn NPK di 0x01)
#define REG_DEVICE_ADDRESS  0x07D0 // Register "Device Address" (SEN0604 & SEN0605, sama-sama di 0x07D0)

HardwareSerial RS485Serial(2);
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

  node.begin(CURRENT_ADDRESS, RS485Serial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  Serial.println();
  Serial.println("=== Ubah Slave Address Sensor EC/pH/Temp/Humid (SEN0604) ===");
  Serial.print("Address lama: ");
  Serial.println(CURRENT_ADDRESS);
  Serial.print("Address baru: ");
  Serial.println(NEW_ADDRESS);

  delay(1000);

  uint8_t result = node.writeSingleRegister(REG_DEVICE_ADDRESS, NEW_ADDRESS);

  if (result == node.ku8MBSuccess) {
    Serial.println("Berhasil! Address sensor sudah diubah.");
    Serial.println("Matikan & nyalakan ulang sensor, lalu jalankan 1_scan_address.cpp untuk verifikasi.");
  } else {
    Serial.print("Gagal mengubah address. Kode error Modbus: 0x");
    Serial.println(result, HEX);
    Serial.println("Cek: wiring RS485, baudrate (9600), dan CURRENT_ADDRESS.");
  }
}

void loop() {
  // Tidak ada yang diulang - proses hanya sekali di setup().
}
