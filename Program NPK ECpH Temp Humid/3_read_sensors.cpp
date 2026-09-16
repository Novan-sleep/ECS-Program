/*
 * 3_read_sensors.cpp
 * ESP32 DevKit V4 - Pembacaan Sensor NPK (SEN0605) + EC/pH/Temp/Humid (SEN0604)
 * DFRobot, RS485/Modbus-RTU, baud 9600
 *
 * Prasyarat: sensor EC/pH/Temp/Humid (SEN0604) sudah diubah address-nya
 * lewat 2_change_address.cpp (ke 0x02) sehingga tidak bentrok dengan
 * sensor NPK (SEN0605) yang tetap di address default 0x01.
 *
 * Register map resmi:
 *   SEN0605 (NPK)               : https://wiki.dfrobot.com/sen0605/docs/21024
 *     0x001E - Nitrogen (N)   mg/kg
 *     0x001F - Phosphorus (P) mg/kg
 *     0x0020 - Potassium (K)  mg/kg
 *
 *   SEN0604 (EC/pH/Temp/Humid)  : https://wiki.dfrobot.com/sen0604/docs/20297
 *     0x0000 - Humidity     %RH  (nilai x10)
 *     0x0001 - Temperature  °C   (nilai x10)
 *     0x0002 - EC           µS/cm (nilai langsung)
 *     0x0003 - pH           pH   (nilai x10)
 *
 * Hardware: sama seperti file sebelumnya di folder ini.
 */

#include <Arduino.h>
#include <ModbusMaster.h>

#define RS485_DE_RE_PIN 4
#define MODBUS_BAUD      9600

#define NPK_ADDRESS   0x01   // SEN0605, tetap di address default
#define ECPH_ADDRESS  0x02   // SEN0604, setelah diubah addressnya

#define REG_NPK_START   0x001E  // N, P, K berurutan (3 register)
#define REG_ECPH_START  0x0000  // Humidity, Temperature, EC, pH (4 register)

HardwareSerial RS485Serial(2);
ModbusMaster node;

void preTransmission() {
  digitalWrite(RS485_DE_RE_PIN, HIGH);
}

void postTransmission() {
  digitalWrite(RS485_DE_RE_PIN, LOW);
}

bool readNPK(uint16_t &nitrogen, uint16_t &phosphorus, uint16_t &potassium) {
  node.begin(NPK_ADDRESS, RS485Serial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  uint8_t result = node.readHoldingRegisters(REG_NPK_START, 3);
  if (result != node.ku8MBSuccess) return false;

  nitrogen   = node.getResponseBuffer(0);
  phosphorus = node.getResponseBuffer(1);
  potassium  = node.getResponseBuffer(2);
  return true;
}

bool readECpHTempHumid(float &humidity, float &temperature, uint16_t &ec, float &ph) {
  node.begin(ECPH_ADDRESS, RS485Serial);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  uint8_t result = node.readHoldingRegisters(REG_ECPH_START, 4);
  if (result != node.ku8MBSuccess) return false;

  humidity    = node.getResponseBuffer(0) / 10.0f;
  temperature = node.getResponseBuffer(1) / 10.0f;
  ec          = node.getResponseBuffer(2);
  ph          = node.getResponseBuffer(3) / 10.0f;
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);

  RS485Serial.begin(MODBUS_BAUD, SERIAL_8N1, 16, 17);

  Serial.println();
  Serial.println("=== Pembacaan Sensor NPK (SEN0605) + EC/pH/Temp/Humid (SEN0604) ===");
}

void loop() {
  uint16_t n, p, k;
  if (readNPK(n, p, k)) {
    Serial.println("-- Sensor NPK (SEN0605) --");
    Serial.print("Nitrogen  : "); Serial.print(n); Serial.println(" mg/kg");
    Serial.print("Phosphorus: "); Serial.print(p); Serial.println(" mg/kg");
    Serial.print("Potassium : "); Serial.print(k); Serial.println(" mg/kg");
  } else {
    Serial.println("Gagal membaca sensor NPK");
  }

  delay(200);

  float hum, temp, ph;
  uint16_t ec;
  if (readECpHTempHumid(hum, temp, ec, ph)) {
    Serial.println("-- Sensor EC/pH/Temp/Humid (SEN0604) --");
    Serial.print("Humidity   : "); Serial.print(hum); Serial.println(" %");
    Serial.print("Temperature: "); Serial.print(temp); Serial.println(" C");
    Serial.print("EC         : "); Serial.print(ec); Serial.println(" us/cm");
    Serial.print("pH         : "); Serial.println(ph);
  } else {
    Serial.println("Gagal membaca sensor EC/pH/Temp/Humid");
  }

  Serial.println("--------------------------------------");
  delay(2000);
}
