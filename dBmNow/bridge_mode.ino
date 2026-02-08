/*
 * Bridge mode: Serial-to-MQTT (WiFi Manager, Serial1 RX=21 @ 9600 → MQTT).
 * Compiled with main sketch; entry is setupBridge() / loopBridge() when BRIDGE_PIN is LOW.
 */

#include "esp_event.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "esp_log.h"

#define BRIDGE_SERIAL_RX_PIN  21
#define BRIDGE_SERIAL_TX_PIN  1
#define BRIDGE_SERIAL_BAUD    9600
#define BRIDGE_LINE_BUF_SIZE  512
#define BRIDGE_SERIAL_VIEWER_PORT  8080
#define BRIDGE_SERIAL_VIEWER_SIZE  4096
#define BRIDGE_PREFS_NS       "mqtt"
#define BRIDGE_DEFAULT_BROKER "192.168.1.100"
#define BRIDGE_DEFAULT_PORT   1883
#define BRIDGE_DEFAULT_TOPIC  "Esp32/result"
#define BRIDGE_MQTT_USER_LEN  32
#define BRIDGE_MQTT_PASS_LEN  64
#define BRIDGE_CONFIG_BUTTON_PIN 0
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

WiFiClient   bridgeWifiClient;
PubSubClient bridgeMqttClient(bridgeWifiClient);
Preferences  bridgeMqttPrefs;
WiFiManager  bridgeWifiManager;

char bridgeMqttBroker[64];
char bridgeMqttTopic[96];
char bridgeMqttUser[BRIDGE_MQTT_USER_LEN];
char bridgeMqttPass[BRIDGE_MQTT_PASS_LEN];
int  bridgeMqttPort = BRIDGE_DEFAULT_PORT;

char bridgeLineBuf[BRIDGE_LINE_BUF_SIZE];
int  bridgeLineLen = 0;

String bridgeSerialViewerBuffer;
WebServer bridgeSerialViewerServer(BRIDGE_SERIAL_VIEWER_PORT);
bool bridgeSerialViewerRunning = false;
WebServer bridgeReconfigureServer(80);

static void bridgeSaveMQTTParams() {
  bridgeMqttPrefs.begin(BRIDGE_PREFS_NS, false);
  bridgeMqttPrefs.putString("broker", bridgeMqttBroker);
  bridgeMqttPrefs.putInt("port", bridgeMqttPort);
  bridgeMqttPrefs.putString("topic", bridgeMqttTopic);
  bridgeMqttPrefs.putString("user", bridgeMqttUser);
  bridgeMqttPrefs.putString("pass", bridgeMqttPass);
  bridgeMqttPrefs.end();
}

static void bridgeLoadMQTTParams() {
  bridgeMqttPrefs.begin(BRIDGE_PREFS_NS, true);
  String b = bridgeMqttPrefs.getString("broker", BRIDGE_DEFAULT_BROKER);
  bridgeMqttPort = bridgeMqttPrefs.getInt("port", BRIDGE_DEFAULT_PORT);
  String t = bridgeMqttPrefs.getString("topic", BRIDGE_DEFAULT_TOPIC);
  String u = bridgeMqttPrefs.getString("user", "");
  String p = bridgeMqttPrefs.getString("pass", "");
  bridgeMqttPrefs.end();
  b.toCharArray(bridgeMqttBroker, sizeof(bridgeMqttBroker));
  t.toCharArray(bridgeMqttTopic, sizeof(bridgeMqttTopic));
  u.toCharArray(bridgeMqttUser, sizeof(bridgeMqttUser));
  p.toCharArray(bridgeMqttPass, sizeof(bridgeMqttPass));
}

static void bridgeSerialViewerTask(void* pv) {
  bridgeSerialViewerServer.on("/", []() {
    bridgeSerialViewerServer.send(200, "text/html",
      "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Serial</title></head><body>"
      "<pre id='log' style='background:#1e1e1e;color:#d4d4d4;padding:8px;font-size:12px;overflow:auto;max-height:240px;'>"
      "(incoming serial from bridge RX)"
      "</pre><script>"
      "function refresh(){ var x=new XMLHttpRequest(); x.onreadystatechange=function(){ if(x.readyState===4&&x.status===200){ document.getElementById('log').textContent=x.responseText||'(no data yet)'; }}; x.open('GET','/serial'); x.send(); }"
      "setInterval(refresh,500); refresh();"
      "</script></body></html>");
  });
  bridgeSerialViewerServer.on("/serial", []() {
    bridgeSerialViewerServer.send(200, "text/plain", bridgeSerialViewerBuffer.length() ? bridgeSerialViewerBuffer : "(no data yet)");
  });
  bridgeSerialViewerServer.begin();
  for (;;) {
    bridgeSerialViewerServer.handleClient();
    while (Serial1.available() && bridgeSerialViewerRunning) {
      char c = Serial1.read();
      if (c >= 32 || c == '\n' || c == '\r' || c == '\t') {
        bridgeSerialViewerBuffer += c;
        if (bridgeSerialViewerBuffer.length() > (unsigned)BRIDGE_SERIAL_VIEWER_SIZE)
          bridgeSerialViewerBuffer = bridgeSerialViewerBuffer.substring(bridgeSerialViewerBuffer.length() - BRIDGE_SERIAL_VIEWER_SIZE);
      }
    }
    delay(2);
  }
}

static void bridgeMqttReconnect() {
  if (bridgeMqttClient.connected()) return;
  digitalWrite(LED_BUILTIN, LOW);
  Serial.print("Connecting MQTT...");
  bool ok = (strlen(bridgeMqttUser) > 0 || strlen(bridgeMqttPass) > 0)
    ? bridgeMqttClient.connect("SerialMQTTBridge", bridgeMqttUser, bridgeMqttPass)
    : bridgeMqttClient.connect("SerialMQTTBridge");
  if (ok) {
    Serial.println(" MQTT connected.");
    bridgeMqttClient.publish(bridgeMqttTopic, "SerialMQTTBridge connected", false);
    digitalWrite(LED_BUILTIN, HIGH);
  } else {
    Serial.print(" failed, rc=");
    Serial.println(bridgeMqttClient.state());
  }
}

void setupBridge() {
  esp_log_level_set("wifi", ESP_LOG_NONE);
  esp_log_level_set("wifi_init", ESP_LOG_NONE);
  bridgeLoadMQTTParams();

  char portStr[8];
  snprintf(portStr, sizeof(portStr), "%d", bridgeMqttPort);
  WiFiManagerParameter custom_mqtt_broker("mqtt_broker", "MQTT Broker", bridgeMqttBroker, sizeof(bridgeMqttBroker));
  WiFiManagerParameter custom_mqtt_port("mqtt_port", "MQTT Port", portStr, 8);
  WiFiManagerParameter custom_mqtt_topic("mqtt_topic", "MQTT Topic", bridgeMqttTopic, sizeof(bridgeMqttTopic));
  WiFiManagerParameter custom_mqtt_user("mqtt_user", "MQTT Username", bridgeMqttUser, BRIDGE_MQTT_USER_LEN);
  WiFiManagerParameter custom_mqtt_pass("mqtt_pass", "MQTT Password", bridgeMqttPass, BRIDGE_MQTT_PASS_LEN);

  bridgeWifiManager.addParameter(&custom_mqtt_broker);
  bridgeWifiManager.addParameter(&custom_mqtt_port);
  bridgeWifiManager.addParameter(&custom_mqtt_topic);
  bridgeWifiManager.addParameter(&custom_mqtt_user);
  bridgeWifiManager.addParameter(&custom_mqtt_pass);

  pinMode(BRIDGE_CONFIG_BUTTON_PIN, INPUT_PULLUP);
  bool forcePortal = false;
  for (int i = 0; i < 50; i++) {
    if (i % 10 == 0) Serial.println("Hold BOOT (GPIO0) to open config portal... (" + String(5 - i/10) + " sec)");
    if (digitalRead(BRIDGE_CONFIG_BUTTON_PIN) == LOW) { forcePortal = true; Serial.println("BOOT pressed."); break; }
    delay(100);
  }
  if (!forcePortal) Serial.println("(Or when on WiFi: http://<IP>/reconfigure)");

  if (forcePortal) {
    Serial1.begin(BRIDGE_SERIAL_BAUD, SERIAL_8N1, BRIDGE_SERIAL_RX_PIN, BRIDGE_SERIAL_TX_PIN);
    bridgeSerialViewerBuffer = "";
    bridgeSerialViewerRunning = true;
    xTaskCreatePinnedToCore(bridgeSerialViewerTask, "serialView", 4096, NULL, 1, NULL, 0);
    bridgeWifiManager.setCustomHeadElement(
      "<script>document.addEventListener('DOMContentLoaded',function(){"
      "var d=document.createElement('div'); d.innerHTML='<strong>Incoming serial</strong> — <a href=\\'http://192.168.4.1:8080\\' target=\\'_blank\\'>open</a><br><iframe src=\\'http://192.168.4.1:8080\\' style=\\'width:100%;height:180px;border:0;\\'></iframe>';"
      "document.body.insertBefore(d,document.body.firstChild);});</script>");
    bridgeWifiManager.startConfigPortal("SerialMQTTBridge");
    bridgeSerialViewerRunning = false;
  } else if (!bridgeWifiManager.autoConnect("SerialMQTTBridge")) {
    Serial.println("Config portal failed. Restarting...");
    delay(3000);
    ESP.restart();
  }

  strncpy(bridgeMqttBroker, custom_mqtt_broker.getValue(), sizeof(bridgeMqttBroker) - 1);
  bridgeMqttBroker[sizeof(bridgeMqttBroker) - 1] = '\0';
  bridgeMqttPort = atoi(custom_mqtt_port.getValue());
  if (bridgeMqttPort <= 0) bridgeMqttPort = BRIDGE_DEFAULT_PORT;
  strncpy(bridgeMqttTopic, custom_mqtt_topic.getValue(), sizeof(bridgeMqttTopic) - 1);
  bridgeMqttTopic[sizeof(bridgeMqttTopic) - 1] = '\0';
  strncpy(bridgeMqttUser, custom_mqtt_user.getValue(), sizeof(bridgeMqttUser) - 1);
  bridgeMqttUser[sizeof(bridgeMqttUser) - 1] = '\0';
  strncpy(bridgeMqttPass, custom_mqtt_pass.getValue(), sizeof(bridgeMqttPass) - 1);
  bridgeMqttPass[sizeof(bridgeMqttPass) - 1] = '\0';
  bridgeSaveMQTTParams();

  Serial.println("WiFi connected");
  Serial.println(WiFi.localIP());
  Serial1.begin(BRIDGE_SERIAL_BAUD, SERIAL_8N1, BRIDGE_SERIAL_RX_PIN, BRIDGE_SERIAL_TX_PIN);
  bridgeMqttClient.setServer(bridgeMqttBroker, bridgeMqttPort);
  bridgeMqttClient.setBufferSize(512);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  Serial.print("MQTT ");
  bool mqttOk = (strlen(bridgeMqttUser) > 0 || strlen(bridgeMqttPass) > 0)
    ? bridgeMqttClient.connect("SerialMQTTBridge", bridgeMqttUser, bridgeMqttPass)
    : bridgeMqttClient.connect("SerialMQTTBridge");
  if (mqttOk) {
    bridgeMqttClient.publish(bridgeMqttTopic, "SerialMQTTBridge connected", false);
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.print("connected. First message published to ");
    Serial.println(bridgeMqttTopic);
  } else {
    Serial.print("connect failed (rc=");
    Serial.print(bridgeMqttClient.state());
    Serial.println("), will retry.");
  }
  Serial.println("Reconfigure: hold BOOT at boot, or http://<IP>/reconfigure");

  bridgeReconfigureServer.on("/", []() {
    bridgeReconfigureServer.send(200, "text/html",
      "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head><body style='font-family:sans-serif;padding:16px'>"
      "<h2>SerialMQTTBridge</h2><p><a href='/reconfigure'>Reconfigure WiFi/MQTT</a> (clears WiFi, restarts)</p></body></html>");
  });
  bridgeReconfigureServer.on("/reconfigure", []() {
    bridgeReconfigureServer.send(200, "text/html", "<!DOCTYPE html><html><body><p>Restarting... Connect to AP SerialMQTTBridge.</p></body></html>");
    delay(500);
    bridgeWifiManager.resetSettings();
    delay(500);
    ESP.restart();
  });
  bridgeReconfigureServer.begin();
}

void loopBridge() {
  bridgeReconfigureServer.handleClient();
  if (!bridgeMqttClient.connected()) {
    digitalWrite(LED_BUILTIN, LOW);
    bridgeMqttReconnect();
    delay(1000);
    return;
  }
  bridgeMqttClient.loop();

  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (bridgeLineLen > 0) {
        bridgeLineBuf[bridgeLineLen] = '\0';
        // Only publish lines that look like JSON (transponder 1-way output); skip debug/status noise.
        if (bridgeLineLen >= 2 && bridgeLineBuf[0] == '{' && bridgeLineBuf[bridgeLineLen - 1] == '}' && bridgeMqttClient.connected()) {
          bridgeMqttClient.publish(bridgeMqttTopic, (const uint8_t*)bridgeLineBuf, bridgeLineLen, false);
          digitalWrite(LED_BUILTIN, LOW);
          delay(40);
          digitalWrite(LED_BUILTIN, HIGH);
        }
        bridgeLineLen = 0;
      }
    } else if (bridgeLineLen < (int)(sizeof(bridgeLineBuf) - 1)) {
      bridgeLineBuf[bridgeLineLen++] = c;
    } else {
      bridgeLineLen = 0;
    }
  }
}
