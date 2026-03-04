/*
 * Board2 - LED Controller
 * หน้าที่:
 *   1. รับ LoRa Broadcast จาก Board3 (DHT22) → ตรวจสอบ Auto mode
 *   2. รับ LoRa CMD จาก Board1 (manual_mode) → สั่ง LED
 *   3. เมื่อ LED เปลี่ยนสถานะ → ส่ง LoRa Feedback กลับ Board1
 *   4. แสดงผลบน OLED
 *
 * LoRa Packet Format:
 *   Board3 → Board2        : "SENSOR:T:xx.x,H:xx.x"
 *   Board1 → Board2        : "CMD:MANUAL:0" หรือ "CMD:MANUAL:1"
 *   Board2 → Board1        : "LED:0" หรือ "LED:1"
 */

#include "LoRaWan_APP.h"
#include "Arduino.h"
#include "HT_SSD1306Wire.h"

// ==================== GPIO ====================
#define LED_PIN  2 // LED Pin ตั้งเองนะ

// ==================== OLED ====================
SSD1306Wire oled(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

// ==================== LoRa Config ====================
#define RF_FREQUENCY          923000000
#define LORA_BANDWIDTH        0
#define LORA_SPREADING_FACTOR 7
#define LORA_CODINGRATE       1
#define LORA_PREAMBLE_LENGTH  8
#define LORA_SYMBOL_TIMEOUT   0
#define LORA_FIX_LENGTH_PAYLOAD_ON false
#define LORA_IQ_INVERSION_ON  false
#define BUFFER_SIZE           64

#define TEMP_THRESHOLD 33.0 // ตั้งค่าอุณหภูมิที่ต้องการให้ไฟLEDทำงานอัตโนมัติ

// ==================== State ====================
char     rxpacket[BUFFER_SIZE];
static   RadioEvents_t RadioEvents;
bool     lora_idle   = true;

float    last_temp   = 0;
float    last_humi   = 0;
int16_t  last_rssi   = 0;
bool     manualMode  = false;
bool     ledState    = false;

bool     need_feedback = false;  // รอส่ง feedback กลับ Board1:netpie

// ==================== Vext ====================
void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

// ==================== LED Control ====================
void setLED(bool state, const char* reason) {
  if (ledState == state) return;  // ไม่เปลี่ยนถ้าค่าเดิม
  ledState = state;
  digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  Serial.printf("[LED] %s → %s\n", reason, ledState ? "ON" : "OFF");
  need_feedback = true;  // ส่ง feedback กลับ Board1:netpie ว่า led ปิดหรือเปิด -> ส่งค่า 1 หรือ 0 ออกไป
  updateOLED();
}

// ==================== Auto Mode ====================
void evaluateAuto() {
  if (manualMode) return;
  bool shouldOn = (last_temp >= TEMP_THRESHOLD); //ตั้งค่า เมื่อtemp >= temp ที่ตั้งไว้ บรรทัดที่ 33 (TEMP_THRESHOLD 33.0)
  setLED(shouldOn, "Auto"); // ไฟเปิด
}

// ==================== OLED Update ====================
void updateOLED() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "=== Board2 LED ===");

  oled.setFont(ArialMT_Plain_16);
  oled.drawString(0, 12, "Temp: " + String(last_temp, 1) + " C");
  oled.drawString(0, 30, "Humi: " + String(last_humi, 1) + " %");

  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 50, "Mode: " + String(manualMode ? "MANUAL" : "AUTO") +
                         "  LED: " + String(ledState ? "ON" : "OFF"));
  oled.display();
}

// ==================== Send LoRa Feedback ====================
void sendFeedback() {
  char msg[16];
  snprintf(msg, sizeof(msg), "LED:%d", ledState ? 1 : 0);

  Radio.Sleep();
  delay(10);
  Radio.Send((uint8_t*)msg, strlen(msg));
  Serial.printf("[LoRa TX Feedback] %s\n", msg);
}

// ==================== LoRa RX Done ====================
void OnRxDone(uint8_t* payload, uint16_t size, int16_t rssi, int8_t snr) {
  memcpy(rxpacket, payload, size);
  rxpacket[size] = '\0';
  last_rssi = rssi;
  Radio.Sleep();

  Serial.printf("[LoRa RX] \"%s\" RSSI:%d\n", rxpacket, rssi);

  // รับค่าจาก Board3 (Sensor Broad) -> สั่งเปิด-ปิดไฟโหมด Auto
  if (strncmp(rxpacket, "SENSOR:", 7) == 0) {
    float t = 0, h = 0;
    sscanf(rxpacket + 7, "T:%f,H:%f", &t, &h);
    last_temp = t;
    last_humi = h;
    Serial.printf("[Sensor] T:%.1f H:%.1f\n", t, h);
    evaluateAuto();  // ตรวจ Auto mode
    updateOLED();
  }

  // รับคำสั่ง CMD จาก Board1 //บอร์ด mqtt netpie
  else if (strncmp(rxpacket, "CMD:MANUAL:", 11) == 0) {
    bool newManual = (rxpacket[11] == '1');
    Serial.printf("[CMD] manual_mode → %s\n", newManual ? "ON" : "OFF");

    if (newManual != manualMode) {
      manualMode = newManual;
      if (manualMode) {
        setLED(true, "Manual ON"); //สั่งไฟเปิด - ปิด โหมดแมนนวล
      } else {
        setLED(false, "Manual OFF → Auto"); //ถ้าแมนนวลโหมด ถูกกดปิด ออโต้โหมดถึงจำทำงาน
        evaluateAuto();  // ตรวจ Auto ทันที
      }
    }
  }

  lora_idle = true;
}

// ==================== LoRa TX Done ====================
void OnTxDone() {
  Serial.println("[LoRa TX] Feedback sent → back to RX");
  lora_idle = true;
}

void OnTxTimeout() {
  Serial.println("[LoRa TX] Timeout → back to RX");
  Radio.Sleep();
  lora_idle = true;
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

  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

  RadioEvents.RxDone    = OnRxDone;
  RadioEvents.TxDone    = OnTxDone;
  RadioEvents.TxTimeout = OnTxTimeout;
  Radio.Init(&RadioEvents);
  Radio.SetChannel(RF_FREQUENCY);

  // RX Config
  Radio.SetRxConfig(
    MODEM_LORA, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
    LORA_CODINGRATE, 0, LORA_PREAMBLE_LENGTH,
    LORA_SYMBOL_TIMEOUT, LORA_FIX_LENGTH_PAYLOAD_ON,
    0, true, 0, 0, LORA_IQ_INVERSION_ON, true
  );

  // TX Config
  Radio.SetTxConfig(
    MODEM_LORA, 14, 0, LORA_BANDWIDTH,
    LORA_SPREADING_FACTOR, LORA_CODINGRATE,
    LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
    true, 0, 0, LORA_IQ_INVERSION_ON, 3000
  );

  oled.clear();
  oled.drawString(0, 0, "Board2 Ready!");
  oled.drawString(0, 16, "Waiting LoRa...");
  oled.display();

  Serial.println("Board2 Ready");
}

// ==================== Loop ====================
void loop() {
  // ส่ง Feedback ก่อน (ถ้ามี) แล้วค่อยกลับ RX
  if (need_feedback) {
    need_feedback = false;
    sendFeedback();
    delay(200);
    return;
  }

  if (lora_idle) {
    lora_idle = false;
    Radio.Rx(0);
  }

  Radio.IrqProcess();
}
