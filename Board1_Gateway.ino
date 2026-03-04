/*
 * Board1 - Gateway
 * หน้าที่:
 *   1. รับ LoRa Broadcast จาก Board3 (DHT22) → อัปเดต NETPIE Shadow
 *   2. รับ manual_mode จาก NETPIE Shadow → ส่ง LoRa ไป Board2 (LED)
 *   3. รับ led_state จาก Board2 ผ่าน LoRa → อัปเดต NETPIE Shadow
 *
 * LoRa Packet Format:
 *   Board3 → Board1/Board2 : "SENSOR:T:xx.x,H:xx.x"
 *   Board1 → Board2        : "CMD:MANUAL:0" หรือ "CMD:MANUAL:1"
 *   Board2 → Board1        : "LED:0" หรือ "LED:1"
 */

#include "LoRaWan_APP.h"
#include "Arduino.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ==================== WiFi ====================
#define WIFI_SSID     "xxx"
#define WIFI_PASSWORD "xxx"

// ==================== NETPIE ====================
#define NETPIE_HOST      "mqtt.netpie.io"
#define NETPIE_PORT      1883
#define NETPIE_CLIENT_ID "62704f0f-86ec-4f16-9ec5-f53f05c9d7ea"  //เปลี่ยน
#define NETPIE_TOKEN     "VvgNqAGrjmp4u6D6EVPZEYdpZLTioHyo"      //เปลี่ยน
#define NETPIE_SECRET    "Vo85VMVxrV9T7eignYuCXSnr82FTcBcu"     //เปลี่ยน

#define SHADOW_UPDATE   "@shadow/data/update"
#define SHADOW_GET      "@shadow/data/get"
#define SHADOW_RESPONSE "@shadow/data/response"
#define SHADOW_UPDATED  "@shadow/data/updated"

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

// ==================== State ====================
char     rxpacket[BUFFER_SIZE];
static   RadioEvents_t RadioEvents;
bool     lora_idle    = true;

float    last_temp    = 0;
float    last_humi    = 0;
int16_t  last_rssi    = 0;
bool     last_led     = false;
bool     manualMode   = false;
bool     ignoreNext   = false;

bool     new_sensor   = false;  // มีข้อมูล sensor ใหม่รอ publish
bool     new_led      = false;  // มีข้อมูล LED ใหม่รอ publish
bool     need_send_cmd = false; // มี CMD รอส่ง LoRa ไป Board2

WiFiClient net;
PubSubClient mqttClient(net);

// ==================== WiFi ====================
void connectWiFi() {
  Serial.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi: " + WiFi.localIP().toString());
}

// ==================== Publish Shadow ====================
void publishShadow() {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<256> doc;
  JsonObject data = doc.createNestedObject("data");
  data["temperature"] = serialized(String(last_temp, 1));
  data["humidity"]    = serialized(String(last_humi, 1));
  data["led"]         = last_led;
  data["manual_mode"] = manualMode;
  data["rssi"]        = last_rssi;

  char payload[256];
  serializeJson(doc, payload);
  ignoreNext = true;
  mqttClient.publish(SHADOW_UPDATE, payload);
  Serial.println("Shadow → " + String(payload));
}

// ==================== Send LoRa CMD to Board2 ====================
void sendLoraCmd(bool manual) {
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "CMD:MANUAL:%d", manual ? 1 : 0);

  // หยุดรับก่อนส่ง
  Radio.Sleep();
  delay(10);

  Radio.Send((uint8_t*)cmd, strlen(cmd));
  Serial.printf("[LoRa TX] %s\n", cmd);
}

// ==================== MQTT Callback ====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[512];
  memcpy(msg, payload, min((unsigned int)511, length));
  msg[length] = '\0';
  Serial.printf("[MQTT] %s | %s\n", topic, msg);

  // รับ Shadow Updated (จาก Dashboard)
  if (strcmp(topic, SHADOW_UPDATED) == 0) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, msg)) return;

    if (ignoreNext) {
      ignoreNext = false;
      Serial.println("Self publish → skip");
      return;
    }

    JsonObject data = doc["data"];
    if (data.isNull()) return;

    if (data.containsKey("manual_mode")) {
      bool newManual = (data["manual_mode"].as<int>() != 0);
      if (newManual != manualMode) {
        manualMode    = newManual;
        need_send_cmd = true;  // ส่ง LoRa ไป Board2
        Serial.printf("[NETPIE] manual_mode → %s\n", manualMode ? "ON" : "OFF");
      }
    }
    return;
  }

  // Boot sync
  if (strcmp(topic, SHADOW_RESPONSE) == 0) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, msg)) return;
    JsonObject data = doc["data"];
    if (data.isNull()) return;

    if (data.containsKey("manual_mode"))
      manualMode = (data["manual_mode"].as<int>() != 0);
    if (data.containsKey("led"))
      last_led = data["led"].as<bool>();

    Serial.printf("[Boot] Manual:%s LED:%s\n",
      manualMode ? "ON" : "OFF", last_led ? "ON" : "OFF");
  }
}

// ==================== NETPIE Connect ====================
void connectNETPIE() {
  mqttClient.setServer(NETPIE_HOST, NETPIE_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setCallback(mqttCallback);

  Serial.print("Connecting NETPIE");
  while (!mqttClient.connected()) {
    if (mqttClient.connect(NETPIE_CLIENT_ID, NETPIE_TOKEN, NETPIE_SECRET)) {
      Serial.println("\nNETPIE Connected!");
      mqttClient.subscribe(SHADOW_UPDATED);
      mqttClient.subscribe(SHADOW_RESPONSE);
      mqttClient.publish(SHADOW_GET, "{}");
    } else {
      Serial.printf(" Failed rc=%d\n", mqttClient.state());
      delay(2000);
    }
  }
}

// ==================== LoRa RX Done ====================
void OnRxDone(uint8_t* payload, uint16_t size, int16_t rssi, int8_t snr) {
  memcpy(rxpacket, payload, size);
  rxpacket[size] = '\0';
  last_rssi = rssi;
  Radio.Sleep();

  Serial.printf("[LoRa RX] \"%s\" RSSI:%d\n", rxpacket, rssi);

  // รับจาก Board3 (Sensor Broadcast)
  if (strncmp(rxpacket, "SENSOR:", 7) == 0) {
    float t = 0, h = 0;
    sscanf(rxpacket + 7, "T:%f,H:%f", &t, &h);
    last_temp  = t;
    last_humi  = h;
    new_sensor = true;
    Serial.printf("[Sensor] T:%.1f H:%.1f\n", t, h);
  }

  // รับจาก Board2 (LED state feedback)
  else if (strncmp(rxpacket, "LED:", 4) == 0) {
    last_led = (rxpacket[4] == '1');
    new_led  = true;
    Serial.printf("[LED Feedback] %s\n", last_led ? "ON" : "OFF");
  }

  lora_idle = true;
}

// ==================== LoRa TX Done ====================
void OnTxDone() {
  Serial.println("[LoRa TX] Done → back to RX");
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
  connectWiFi();
  connectNETPIE();

  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

  RadioEvents.RxDone   = OnRxDone;
  RadioEvents.TxDone   = OnTxDone;
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

  Serial.println("Board1 Ready");
}

// ==================== Loop ====================
void loop() {
  if (!mqttClient.connected()) connectNETPIE();

  // ส่ง CMD LoRa ไป Board2 (ถ้ามี)
  if (need_send_cmd) {
    need_send_cmd = false;
    sendLoraCmd(manualMode); //ส่ง manualMode -> LoRa Node#2 
    delay(200);  // รอ TX เสร็จก่อน
    return;
  }

  // กลับมา RX mode
  if (lora_idle) {
    lora_idle = false;
    Radio.Rx(0);
  }

  // Publish sensor ใหม่
  if (new_sensor) {
    new_sensor = false;
    publishShadow();
  }

  // Publish LED state ใหม่
  if (new_led) {
    new_led = false;
    publishShadow();
  }

  mqttClient.loop();
  Radio.IrqProcess();
}
