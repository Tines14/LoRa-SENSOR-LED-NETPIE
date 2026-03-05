/*
 * Board2 - LED Controller (RadioLib)
 * หน้าที่:
 *   1. รับ LoRa จาก Board3 (Sensor) → ตรวจสอบ Auto mode
 *   2. รับ LoRa CMD จาก Board1 → สั่ง LED
 *   3. ส่ง LoRa Feedback กลับ Board1
 *   4. แสดงผลบน OLED
 *
 * LoRa Packet Format:
 *   Board3 → Board2 : "SENSOR:M:xx.x,T:xx.x"
 *   Board1 → Board2 : "CMD:MANUAL:1/0" หรือ "CMD:AUTO:1/0"
 *   Board2 → Board1 : "FB:LED:x,MODE:manual/auto/off"
 *
 * Logic:
 *   CMD:MANUAL:1 → เปิด LED, ปิด Auto อัตโนมัติ
 *   CMD:MANUAL:0 → ปิด LED (ออกจาก Manual)
 *   CMD:AUTO:1   → เปิด Auto, ปิด Manual อัตโนมัติ
 *   CMD:AUTO:0   → ปิด Auto
 *   Auto active  → ดูค่า moisture threshold สั่ง LED เอง
 *   Manual active → Auto ไม่ทำงานจนกว่าจะกด Auto(1)
 */

#include <RadioLib.h>
#include "Arduino.h"
#include <Wire.h>
#include "HT_SSD1306Wire.h"

// ==================== GPIO ====================
#define LED_PIN  2

// ==================== OLED ====================
SSD1306Wire oled(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

// ==================== LoRa (Heltec ESP32 V3) ====================
SX1262 radio = new Module(8, 14, 12, 13);

// ==================== Threshold ====================
#define MOIS_THRESHOLD 20.0

// ==================== State ====================
// sysMode: 0=off, 1=manual, 2=auto
int   sysMode  = 0;
bool  ledState = false;
float last_mois = 0;
float last_temp = 0;
int   last_rssi = 0;

// ==================== Vext ====================
void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

// ==================== OLED Update ====================
void updateOLED() {
  String modeStr = "OFF";
  if (sysMode == 1) modeStr = "MANUAL";
  else if (sysMode == 2) modeStr = "AUTO";

  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "=== Board2 LED ===");
  oled.setFont(ArialMT_Plain_16);
  oled.drawString(0, 12, "Mois: " + String(last_mois, 1) + " %");
  oled.drawString(0, 30, "Temp: " + String(last_temp, 1) + " C");
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 50, "Mode:" + modeStr + " LED:" + String(ledState ? "ON" : "OFF"));
  oled.display();
}

// ==================== ส่ง Feedback กลับ Board1 ====================
void sendFeedback() {
  const char* modeStr = "off";
  if (sysMode == 1)      modeStr = "manual";
  else if (sysMode == 2) modeStr = "auto";

  char msg[48];
  snprintf(msg, sizeof(msg), "FB:LED:%d,MODE:%s", ledState ? 1 : 0, modeStr);

  delay(50);
  int state = radio.transmit((uint8_t*)msg, strlen(msg));
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa TX] \"%s\" OK\n", msg);
  } else {
    Serial.printf("[LoRa TX] Failed! Error: %d\n", state);
  }
}

// ==================== ตั้งค่า LED + ส่ง Feedback ====================
void setLED(bool state, const char* reason) {
  ledState = state;
  digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  Serial.printf("[LED] %s → %s  Mode:%d\n", reason, ledState ? "ON" : "OFF", sysMode);
  sendFeedback();
  updateOLED();
}

// ==================== Auto Evaluate ====================
void evaluateAuto() {
  if (sysMode != 2) return;
  bool shouldOn = (last_mois >= MOIS_THRESHOLD);
  // ส่ง feedback ทุกครั้งที่ sensor อัปเดต ไม่ว่า LED จะเปลี่ยนหรือไม่
  ledState = shouldOn;
  digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  Serial.printf("[Auto] Mois:%.1f → LED:%s\n", last_mois, ledState ? "ON" : "OFF");
  sendFeedback();
  updateOLED();
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  VextON();
  delay(100);

  oled.init();
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "Board2 Starting...");
  oled.display();

  Serial.print("LoRa Init ... ");
  int state = radio.begin(915.0, 125.0, 7, 5, 0x12, 22, 8);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("OK!");
  } else {
    Serial.printf("FAILED! Error: %d\n", state);
    oled.clear();
    oled.drawString(0, 0, "LoRa FAILED!");
    oled.drawString(0, 16, "Error: " + String(state));
    oled.display();
    while (true);
  }

  oled.clear();
  oled.drawString(0, 0, "Board2 Ready!");
  oled.drawString(0, 16, "Waiting LoRa...");
  oled.display();

  Serial.println("Board2 Ready รอรับ LoRa...");
}

// ==================== Loop ====================
void loop() {
  char rxpacket[64] = {0};
  int state = radio.receive((uint8_t*)rxpacket, sizeof(rxpacket) - 1);

  if (state == RADIOLIB_ERR_NONE) {
    last_rssi = (int)radio.getRSSI();
    Serial.printf("[LoRa RX] \"%s\"  RSSI:%d dBm\n", rxpacket, last_rssi);

    // ---- Sensor จาก Board3 ----
    if (strncmp(rxpacket, "SENSOR:", 7) == 0) {
      float m = 0, t = 0;
      sscanf(rxpacket + 7, "M:%f,T:%f", &m, &t);
      last_mois = m;
      last_temp = t;
      Serial.printf("[Sensor] Mois:%.1f%%  Temp:%.1fC\n", m, t);
      evaluateAuto();   // Auto ทำงานถ้า sysMode==2
      updateOLED();
    }

    // ---- CMD:MANUAL:1/0 จาก Board1 ----
    else if (strncmp(rxpacket, "CMD:MANUAL:", 11) == 0) {
      int val = rxpacket[11] - '0';
      Serial.printf("[CMD] MANUAL:%d\n", val);

      if (val == 1) {
        // เปิด Manual → ปิด Auto อัตโนมัติ
        sysMode = 1;
        setLED(true, "Manual ON");
      } else {
        // ปิด Manual → ปิด LED
        if (sysMode == 1) sysMode = 0;
        setLED(false, "Manual OFF");
      }
    }

    // ---- CMD:AUTO:1/0 จาก Board1 ----
    else if (strncmp(rxpacket, "CMD:AUTO:", 9) == 0) {
      int val = rxpacket[9] - '0';
      Serial.printf("[CMD] AUTO:%d\n", val);

      if (val == 1) {
        // เปิด Auto → ปิด Manual อัตโนมัติ
        sysMode = 2;
        Serial.println("[Auto] Mode ON → evaluate now");
        evaluateAuto();  // ประเมินทันทีจากค่า sensor ล่าสุด
      } else {
        // ปิด Auto → ปิด LED
        if (sysMode == 2) sysMode = 0;
        setLED(false, "Auto OFF");
      }
    }

  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    // ปกติ วนรับใหม่
  } else {
    Serial.printf("[LoRa RX] Error: %d\n", state);
  }
}
