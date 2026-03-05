/*
 * Board1 - Gateway (RadioLib + NETPIE)
 * หน้าที่:
 *   1. รับคำสั่ง Manual / Auto จาก NETPIE Dashboard
 *   2. ส่ง LoRa CMD ไป Board2
 *   3. รับ LoRa Feedback จาก Board2 → อัปเดต NETPIE Shadow
 *   4. รับ Sensor data จาก Board3 → อัปเดต NETPIE Shadow
 *   5. แสดงผลบน OLED
 *
 * LoRa Packet Format:
 *   Board1 → Board2 : "CMD:MANUAL:1/0" หรือ "CMD:AUTO:1/0"
 *   Board2 → Board1 : "FB:LED:x,MODE:manual/auto"
 *   Board3 → Board1 : "SENSOR:M:xx.x,T:xx.x"
 *
 * NETPIE Shadow keys:
 *   moisture, temperature, led, mode (manual/auto/off), rssi
 *   รับคำสั่ง: manual_cmd (0/1), auto_cmd (0/1)
 */

#include <RadioLib.h>
#include "Arduino.h"
#include <Wire.h>
#include "HT_SSD1306Wire.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ==================== WiFi ====================
#define WIFI_SSID     "Tlxns" //"Project_IOT"
#define WIFI_PASSWORD "tine14022002"  //"1234567890"

// ==================== NETPIE ====================
#define NETPIE_HOST      "mqtt.netpie.io"
#define NETPIE_PORT      1883
#define NETPIE_CLIENT_ID "62704f0f-86ec-4f16-9ec5-f53f05c9d7ea"
#define NETPIE_TOKEN     "VvgNqAGrjmp4u6D6EVPZEYdpZLTioHyo"
#define NETPIE_SECRET    "Vo85VMVxrV9T7eignYuCXSnr82FTcBcu"

#define SHADOW_UPDATE   "@shadow/data/update"
#define SHADOW_GET      "@shadow/data/get"
#define SHADOW_RESPONSE "@shadow/data/response"
#define SHADOW_UPDATED  "@shadow/data/updated"

// ==================== OLED ====================
SSD1306Wire oled(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

// ==================== LoRa (Heltec ESP32 V3) ====================
SX1262 radio = new Module(8, 14, 12, 13);

// ==================== Vext ====================
void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

// ==================== State ====================
float  last_mois  = 0;
float  last_temp  = 0;
int    last_rssi  = 0;
int    last_led   = 0;
// mode: 0=off, 1=manual, 2=auto
int    sysMode    = 0;

bool   need_send_manual = false;
bool   need_send_auto   = false;
int    pending_manual   = 0;
int    pending_auto     = 0;

bool   ignoreNext = false;

WiFiClient   net;
PubSubClient mqttClient(net);

// ==================== OLED Update ====================
void updateOLED() {
  String modeStr = "OFF";
  if (sysMode == 1) modeStr = "MANUAL";
  else if (sysMode == 2) modeStr = "AUTO";

  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "=== Board1 GW ===");
  oled.setFont(ArialMT_Plain_16);
  oled.drawString(0, 12, "Mois: " + String(last_mois, 1) + " %");
  oled.drawString(0, 30, "Temp: " + String(last_temp, 1) + " C");
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 50, "Mode:" + modeStr + " LED:" + String(last_led ? "ON" : "OFF"));
  oled.display();
}

// ==================== Publish Shadow ====================
void publishShadow() {
  if (!mqttClient.connected()) return;

  String modeStr = "off";
  if (sysMode == 1) modeStr = "manual";
  else if (sysMode == 2) modeStr = "auto";

  // manual_cmd / auto_cmd สะท้อน mode ปัจจุบันกลับ Dashboard
  int manual_cmd_val = (sysMode == 1) ? 1 : 0;
  int auto_cmd_val   = (sysMode == 2) ? 1 : 0;

  StaticJsonDocument<320> doc;
  JsonObject data = doc.createNestedObject("data");
  data["moisture"]    = serialized(String(last_mois, 1));
  data["temperature"] = serialized(String(last_temp, 1));
  data["led"]         = last_led;
  data["mode"]        = modeStr;
  data["rssi"]        = last_rssi;
  data["manual_cmd"]  = manual_cmd_val;
  data["auto_cmd"]    = auto_cmd_val;

  char payload[320];
  serializeJson(doc, payload);
  ignoreNext = true;
  mqttClient.publish(SHADOW_UPDATE, payload);
  Serial.println("[NETPIE] Shadow → " + String(payload));
}

// ==================== Send LoRa CMD + รอรับ Feedback ====================
void sendLoraCmd(const char* cmd) {
  int state = radio.transmit((uint8_t*)cmd, strlen(cmd));
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa TX] Failed! Error: %d\n", state);
    return;
  }
  Serial.printf("[LoRa TX] \"%s\" OK\n", cmd);

  // รอ Feedback จาก Board2 (timeout 3 วินาที)
  char rxpacket[64] = {0};
  unsigned long t0 = millis();
  while (millis() - t0 < 3000) {
    memset(rxpacket, 0, sizeof(rxpacket));
    state = radio.receive((uint8_t*)rxpacket, sizeof(rxpacket) - 1);

    if (state == RADIOLIB_ERR_NONE) {
      Serial.printf("[LoRa RX Feedback] \"%s\"\n", rxpacket);
      // "FB:LED:x,MODE:manual/auto"
      if (strncmp(rxpacket, "FB:", 3) == 0) {
        int ledVal = 0;
        char modeBuf[16] = {0};
        sscanf(rxpacket, "FB:LED:%d,MODE:%15s", &ledVal, modeBuf);
        last_led = ledVal;
        if (strcmp(modeBuf, "manual") == 0)     sysMode = 1;
        else if (strcmp(modeBuf, "auto") == 0)  sysMode = 2;
        else                                     sysMode = 0;
        Serial.printf("[FB] LED:%d MODE:%s\n", last_led, modeBuf);
        publishShadow();
        updateOLED();
        break;
      }
    } else if (state != RADIOLIB_ERR_RX_TIMEOUT) {
      Serial.printf("[LoRa RX] Error: %d\n", state);
      break;
    }
  }
}

// ==================== MQTT Callback ====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[512];
  memcpy(msg, payload, min((unsigned int)511, length));
  msg[length] = '\0';
  Serial.printf("[MQTT] %s | %s\n", topic, msg);

  // --- อัปเดตจาก Dashboard ---
  if (strcmp(topic, SHADOW_UPDATED) == 0) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, msg)) return;

    // ถ้าเป็น self-publish → skip แต่ reset flag แล้วออก
    if (ignoreNext) {
      ignoreNext = false;
      Serial.println("[MQTT] Self publish → skip");
      return;
    }

    JsonObject data = doc["data"];
    if (data.isNull()) return;

    bool hasCmd = false;

    // รับคำสั่ง manual_cmd (รองรับทั้ง int 0/1 และ boolean true/false)
    if (data.containsKey("manual_cmd")) {
      int val = data["manual_cmd"].as<int>();
      Serial.printf("[NETPIE] manual_cmd = %d  (sysMode=%d)\n", val, sysMode);
      int cur = (sysMode == 1) ? 1 : 0;
      if (val != cur) {
        pending_manual   = val;
        need_send_manual = true;
        hasCmd = true;
      }
    }

    // รับคำสั่ง auto_cmd (รองรับทั้ง int 0/1 และ boolean true/false)
    if (data.containsKey("auto_cmd")) {
      int val = data["auto_cmd"].as<int>();
      Serial.printf("[NETPIE] auto_cmd = %d  (sysMode=%d)\n", val, sysMode);
      int cur = (sysMode == 2) ? 1 : 0;
      if (val != cur) {
        pending_auto   = val;
        need_send_auto = true;
        hasCmd = true;
      }
    }

    if (hasCmd) Serial.println("[MQTT] Command queued");
    return;
  }

  // --- โหลด state ตอน boot ---
  if (strcmp(topic, SHADOW_RESPONSE) == 0) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, msg)) return;
    JsonObject data = doc["data"];
    if (data.isNull()) return;

    if (data.containsKey("led"))  last_led = data["led"].as<int>();
    if (data.containsKey("mode")) {
      const char* m = data["mode"].as<const char*>();
      if (strcmp(m, "manual") == 0)     sysMode = 1;
      else if (strcmp(m, "auto") == 0)  sysMode = 2;
      else                               sysMode = 0;
    }
    Serial.printf("[Boot] Mode:%d LED:%d\n", sysMode, last_led);
    updateOLED();
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

// ==================== WiFi Connect ====================
void connectWiFi() {
  Serial.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi: " + WiFi.localIP().toString());
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);

  VextON();
  delay(100);

  oled.init();
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "Board1 Starting...");
  oled.display();

  connectWiFi();

  oled.clear();
  oled.drawString(0, 0, "WiFi OK");
  oled.drawString(0, 16, WiFi.localIP().toString());
  oled.display();

  connectNETPIE();

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
  oled.drawString(0, 0, "Board1 Ready!");
  oled.display();
  delay(1000);
  updateOLED();

  Serial.println("Board1 Ready");
}

// ==================== Loop ====================
void loop() {
  if (!mqttClient.connected()) connectNETPIE();
  mqttClient.loop();

  // --- ส่ง CMD:MANUAL ---
  if (need_send_manual) {
    need_send_manual = false;
    // ถ้ากด manual(1) → ปิด auto อัตโนมัติ
    if (pending_manual == 1) {
      sysMode = 1;
      char cmd[32];
      snprintf(cmd, sizeof(cmd), "CMD:MANUAL:1");
      sendLoraCmd(cmd);
    } else {
      // manual(0) → ปิด LED
      if (sysMode == 1) sysMode = 0;
      char cmd[32];
      snprintf(cmd, sizeof(cmd), "CMD:MANUAL:0");
      sendLoraCmd(cmd);
    }
    updateOLED();
  }

  // --- ส่ง CMD:AUTO ---
  if (need_send_auto) {
    need_send_auto = false;
    // ถ้ากด auto(1) → ปิด manual อัตโนมัติ
    if (pending_auto == 1) {
      sysMode = 2;
      char cmd[32];
      snprintf(cmd, sizeof(cmd), "CMD:AUTO:1");
      sendLoraCmd(cmd);
    } else {
      // auto(0) → ปิด auto
      if (sysMode == 2) sysMode = 0;
      char cmd[32];
      snprintf(cmd, sizeof(cmd), "CMD:AUTO:0");
      sendLoraCmd(cmd);
    }
    updateOLED();
  }

  // --- รับ LoRa ปกติ (Sensor จาก Board3) ---
  char rxpacket[64] = {0};
  int state = radio.receive((uint8_t*)rxpacket, sizeof(rxpacket) - 1);

  if (state == RADIOLIB_ERR_NONE) {
    last_rssi = (int)radio.getRSSI();
    Serial.printf("[LoRa RX] \"%s\"  RSSI: %d dBm\n", rxpacket, last_rssi);

    if (strncmp(rxpacket, "SENSOR:", 7) == 0) {
      float m = 0, t = 0;
      sscanf(rxpacket + 7, "M:%f,T:%f", &m, &t);
      last_mois = m;
      last_temp = t;
      Serial.printf("[Sensor] Mois:%.1f%%  Temp:%.1f C\n", m, t);
      publishShadow();
      updateOLED();
    }

    // Feedback ที่ไม่ได้รับในช่วง sendLoraCmd
    else if (strncmp(rxpacket, "FB:", 3) == 0) {
      int ledVal = 0;
      char modeBuf[16] = {0};
      sscanf(rxpacket, "FB:LED:%d,MODE:%15s", &ledVal, modeBuf);
      last_led = ledVal;
      if (strcmp(modeBuf, "manual") == 0)     sysMode = 1;
      else if (strcmp(modeBuf, "auto") == 0)  sysMode = 2;
      else                                     sysMode = 0;
      publishShadow();
      updateOLED();
    }

  } else if (state != RADIOLIB_ERR_RX_TIMEOUT) {
    Serial.printf("[LoRa RX] Error: %d\n", state);
  }
}
