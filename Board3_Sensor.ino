/*
 * Board3 - Sensor Sender
 * หน้าที่:
 *   อ่าน DHT22 ทุก 10 วินาที → Broadcast LoRa ไปพร้อมกันทั้ง Board1:netpie และ Board2: Control LED
 *
 * LoRa Packet Format:
 *   "SENSOR:T:xx.x,H:xx.x"
 */

#include "LoRaWan_APP.h"
#include "Arduino.h"
#include <DHT.h>

// ==================== DHT22 ====================
#define DHT_PIN  4 // ขา dht 22 เปลี่ยนเองนะ
#define DHT_TYPE DHT22

DHT dht(DHT_PIN, DHT_TYPE);

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

#define SEND_INTERVAL 10000  // 10 วินาที

// ==================== State ====================
static RadioEvents_t RadioEvents;
bool     lora_idle   = true;
unsigned long lastSend = 0;

// ==================== LoRa TX Done ====================
void OnTxDone() {
  Serial.println("[LoRa TX] Done");
  lora_idle = true;
}

void OnTxTimeout() {
  Serial.println("[LoRa TX] Timeout");
  Radio.Sleep();
  lora_idle = true;
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  dht.begin();

  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

  RadioEvents.TxDone    = OnTxDone;
  RadioEvents.TxTimeout = OnTxTimeout;
  Radio.Init(&RadioEvents);
  Radio.SetChannel(RF_FREQUENCY);

  Radio.SetTxConfig(
    MODEM_LORA, 14, 0, LORA_BANDWIDTH,
    LORA_SPREADING_FACTOR, LORA_CODINGRATE,
    LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
    true, 0, 0, LORA_IQ_INVERSION_ON, 3000
  );

  Serial.println("Board3 Sensor Ready");
}

// ==================== Loop ====================
void loop() {
  if (lora_idle && (millis() - lastSend >= SEND_INTERVAL)) {
    lastSend = millis();

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      char packet[BUFFER_SIZE];
      snprintf(packet, sizeof(packet), "SENSOR:T:%.1f,H:%.1f", t, h); //format ที่ส่ง

      lora_idle = false;
      Radio.Send((uint8_t*)packet, strlen(packet));
      Serial.printf("[LoRa Broadcast] %s\n", packet);
    } else {
      Serial.println("DHT22 read failed");
    }
  }

  Radio.IrqProcess();
}
