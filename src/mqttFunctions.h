
#include <WiFi.h>
#include <Arduino.h>
#include "mEEPROM.h"

extern "C" {
	#include "freertos/FreeRTOS.h"
	#include "freertos/timers.h"
}
//#include <AsyncMqttClient.h>
#include <PsychicMqttClient.h>
#define MAX_PENDING_MSGS 10
#define MQTT_BUFFER_SIZE 2048
const unsigned long MQTT_TIMEOUT_MS = 5000;  

PsychicMqttClient mqttClient;

TimerHandle_t mqttReconnectTimer;
//mEEPROM pref;
bool mqttEnabled = false;
bool haDiscoveryEnabled = true;  // Enable Home Assistant MQTT Discovery
String sUser;
String sPass;
String sServer;
String sSubscribe;
String sTopic;
String sTopicData;
String sClientid;
uint16_t iPort = 1883; // Default MQTT Port

char buffer[10];

typedef struct 
{
  char *payloadbuffer;
  char *topicbuffer;
  int msg_id;
  uint32_t millis;
  bool active;
} mqtt_msg_t;

static mqtt_msg_t pending_msgs[MAX_PENDING_MSGS];

bool mqttPublish(String topic, String payload, bool retain)
{
  return mqttPublish(topic.c_str(),payload.c_str(),retain);
}

bool mqttPublish(const char* topic, const char* payload, bool retain)
{
  if (!mqttClient.connected()) return false;

  // Remove from pending messages
  for (int i = 0; i < MAX_PENDING_MSGS; i++) {
    if (pending_msgs[i].active && millis() - pending_msgs[i].millis > MQTT_TIMEOUT_MS) {
      free(pending_msgs[i].payloadbuffer);
      free(pending_msgs[i].topicbuffer);
      log_d("Time out, removed msg_id %d from pending messages", pending_msgs[i].msg_id);
      pending_msgs[i].payloadbuffer = nullptr;
      pending_msgs[i].topicbuffer = nullptr;
      pending_msgs[i].msg_id = -1;
      pending_msgs[i].active = false;
      
    }
  }

  size_t lenPayload = strlen(payload);
  size_t lenTopic = strlen(topic);

  char *payloadBuffer = (char*)malloc(lenPayload+1);
  char *topicBuffer = (char*)malloc(lenTopic+1);
  if (!payloadBuffer || !topicBuffer) {
      log_e("Failed to allocate memory");
      if (payloadBuffer) free(payloadBuffer);
      if (topicBuffer) free(topicBuffer);
      return false;
  }

  bool addedToQueue = false;
  strcpy(payloadBuffer, payload);
  strcpy(topicBuffer, topic);
  int msg_id = mqttClient.publish(topicBuffer,1,retain,payloadBuffer,lenPayload,true);

  if (msg_id < 0) {
      log_e("Failed to enqueue message");
      free(payloadBuffer);
      free(topicBuffer);
      return false;
  } 

  // Track buffer for cleanup
  for (int i = 0; i < MAX_PENDING_MSGS; i++) {
      if (!pending_msgs[i].active) {
          pending_msgs[i].payloadbuffer = payloadBuffer;
          pending_msgs[i].topicbuffer = topicBuffer;
          pending_msgs[i].msg_id = msg_id;
          pending_msgs[i].millis = millis();
          pending_msgs[i].active = true;
          addedToQueue = true;
          break;
      }
  }
  if (!addedToQueue) {
      log_e("No space in pending messages");
      free(payloadBuffer);
      free(topicBuffer);
      return false;
  }
  return true;
}

bool sendUpdateMQTTData()
{
   if (mqttClient.connected())
   {
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/SOCTrickEnable").c_str(), (Inverter.EnableSOCTrick() == true) ? "ON" : "OFF",true);
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/RequestFlagsEnable").c_str(), (Inverter.EnableRequestFlags() == true) ? "ON" : "OFF",true);
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/ForceCharge").c_str(), (Inverter.ForceCharge() == true) ? "ON" : "OFF" , true);  
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/DischargeEnable").c_str(), (Inverter.DischargeEnable() == true && Inverter.ManualAllowDischarge()) ? "ON" : "OFF" , true); 
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/ChargeEnable").c_str(), (Inverter.ChargeEnable() == true && Inverter.ManualAllowCharge()) ? "ON" : "OFF" , true);
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/SmartCharge").c_str(), (Inverter.AutoCharge() == true) ? "ON" : "OFF" , true);
    return true;
  } 
  else  
  {
    log_e("MQTT not connected, cannot send update data.");
    return false;
  }
}

//
// Send VE data from passive mode to MQTT
//
bool sendVE2MQTT() {
  /*
  for (int i = 0; i < veHandle.FrameLength(); i++) {
    String key = veHandle.veName[i];
    if (key.length() == 0)
      break; // stop is no key is here.
    String value = veHandle.veValue[i];
    String topic = wifiManager.GetMQTTTopic() + "/" + key;
    if (mqttClient.connected()) {
      topic.replace("#", ""); // # in a topic is a no go for MQTT
      value.replace("\r\n", "");
      if (mqttPublish(topic.c_str(), value.c_str(),true)) {
        log_i("MQTT message sent succesfully: %s: \"%s\"", topic.c_str(), value.c_str());
        } else {
        log_e("Sending MQTT message failed: %s: %s", topic.c_str(), value.c_str());
        }
    } 
  } */
  // Publish Json with more details in.
  mqttPublish((sTopic + "/Data").c_str(),generateDatatoJSON(false).c_str(),false);
  // Send Voltage
  sprintf (buffer, "%u", Inverter.BattVoltage());
  mqttPublish((sTopic + "/V").c_str(),buffer,false);
  // Send Current
  sprintf (buffer, "%i", Inverter.BattCurrentmA());
  mqttPublish((sTopic + "/I").c_str(),buffer,false);
  // Send SOC
  sprintf (buffer, "%i", Inverter.BattSOC());
  mqttPublish((sTopic + "/SOC").c_str(),buffer,false);

  return true;
}

// Publish Home Assistant MQTT Discovery messages
void publishHADiscovery() {
  if (!haDiscoveryEnabled || !mqttClient.connected()) return;
  
  String deviceId = WiFi.macAddress();
  deviceId.replace(":", "");
  String baseTopic = "homeassistant";
  String nodeId = "diybatterybms_" + deviceId;
  
  // Device info (shared across all entities)
  String deviceJson = String(",\"device\":{\"identifiers\":[\"diybatterybms_") + deviceId + 
                      String("\"],\"name\":\"DIY Battery BMS\",\"model\":\"ESP32 BMS\",\"manufacturer\":\"https://github.com/sijones/DiyBatteryBMS\"}");
  
  log_i("Publishing Home Assistant Discovery configs...");
  WS_LOG_I("Publishing Home Assistant Discovery configs...");
  
  // Battery SOC Sensor
  mqttPublish((baseTopic + "/sensor/" + nodeId + "_soc/config").c_str(),
    (String("{\"name\":\"Battery SOC\",\"unique_id\":\"") + nodeId + "_soc\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{{ value_json.battsoc }}\"," +
     "\"unit_of_measurement\":\"%\",\"device_class\":\"battery\",\"state_class\":\"measurement\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Battery Voltage Sensor
  mqttPublish((baseTopic + "/sensor/" + nodeId + "_voltage/config").c_str(),
    (String("{\"name\":\"Battery Voltage\",\"unique_id\":\"") + nodeId + "_voltage\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
    "\"value_template\":\"{{ value_json.battvoltage | multiply(0.01) | round(1) }}\"," +
     "\"unit_of_measurement\":\"V\",\"device_class\":\"voltage\",\"state_class\":\"measurement\"," +
     "\"suggested_display_precision\":1" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Battery Current Sensor
  mqttPublish((baseTopic + "/sensor/" + nodeId + "_current/config").c_str(),
    (String("{\"name\":\"Battery Current\",\"unique_id\":\"") + nodeId + "_current\"," +
    "\"state_topic\":\"" + sTopic + "/Data\"," +
    "\"value_template\":\"{{ value_json.battcurrent | multiply(0.1) | round(1) }}\"," +
    "\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\"" +
    "\"suggested_display_precision\":1" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Charge Current Sensor
  mqttPublish((baseTopic + "/sensor/" + nodeId + "_chargecurrent/config").c_str(),
    (String("{\"name\":\"Charge Current Limit\",\"unique_id\":\"") + nodeId + "_chargecurrent\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{{ value_json.chargecurrent | multiply(0.001) | round(1) }}\"," +
     "\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Discharge Current Sensor
  mqttPublish((baseTopic + "/sensor/" + nodeId + "_dischargecurrent/config").c_str(),
    (String("{\"name\":\"Discharge Current Limit\",\"unique_id\":\"") + nodeId + "_dischargecurrent\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{{ value_json.dischargecurrent | multiply(0.001) | round(1) }}\"," +
     "\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Charge Adjust Sensor
  mqttPublish((baseTopic + "/sensor/" + nodeId + "_chargeadjust/config").c_str(),
    (String("{\"name\":\"Charge Adjust\",\"unique_id\":\"") + nodeId + "_chargeadjust\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{{ value_json.chargeadjust | multiply(0.001) | round(2) }}\"," +
     "\"state_class\":\"measurement\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Free Heap Sensor
  mqttPublish((baseTopic + "/sensor/" + nodeId + "_freeheap/config").c_str(),
    (String("{\"name\":\"Free Heap\",\"unique_id\":\"") + nodeId + "_freeheap\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{{ value_json.freeheap }}\"," +
     "\"unit_of_measurement\":\"B\",\"entity_category\":\"diagnostic\",\"state_class\":\"measurement\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Charge Enable Binary Sensor
  mqttPublish((baseTopic + "/binary_sensor/" + nodeId + "_chargeenabled/config").c_str(),
    (String("{\"name\":\"Charge Enabled Status\",\"unique_id\":\"") + nodeId + "_chargeenabled\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{% if value_json.chargeenabled %}ON{% else %}OFF{% endif %}\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Discharge Enable Binary Sensor
  mqttPublish((baseTopic + "/binary_sensor/" + nodeId + "_dischargeenabled/config").c_str(),
    (String("{\"name\":\"Discharge Enabled Status\",\"unique_id\":\"") + nodeId + "_dischargeenabled\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{% if value_json.dischargeenabled %}ON{% else %}OFF{% endif %}\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Force Charge Binary Sensor
  mqttPublish((baseTopic + "/binary_sensor/" + nodeId + "_forcecharge/config").c_str(),
    (String("{\"name\":\"Force Charge Status\",\"unique_id\":\"") + nodeId + "_forcecharge\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{% if value_json.forcecharge %}ON{% else %}OFF{% endif %}\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Smart Charge Binary Sensor
  mqttPublish((baseTopic + "/binary_sensor/" + nodeId + "_smartcharge/config").c_str(),
    (String("{\"name\":\"Smart Charge Status\",\"unique_id\":\"") + nodeId + "_smartcharge\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{% if value_json.autocharge %}ON{% else %}OFF{% endif %}\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Charge Enable Switch
  mqttPublish((baseTopic + "/switch/" + nodeId + "_charge/config").c_str(),
    (String("{\"name\":\"Charge Enable\",\"unique_id\":\"") + nodeId + "_charge\"," +
     "\"state_topic\":\"" + sTopic + "/Param/ChargeEnable\"," +
     "\"command_topic\":\"" + sTopic + "/set/ChargeEnable\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Discharge Enable Switch
  mqttPublish((baseTopic + "/switch/" + nodeId + "_discharge/config").c_str(),
    (String("{\"name\":\"Discharge Enable\",\"unique_id\":\"") + nodeId + "_discharge\"," +
     "\"state_topic\":\"" + sTopic + "/Param/DischargeEnable\"," +
     "\"command_topic\":\"" + sTopic + "/set/DischargeEnable\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Force Charge Switch
  mqttPublish((baseTopic + "/switch/" + nodeId + "_forcecharge/config").c_str(),
    (String("{\"name\":\"Force Charge\",\"unique_id\":\"") + nodeId + "_forcecharge\"," +
     "\"state_topic\":\"" + sTopic + "/Param/ForceCharge\"," +
     "\"command_topic\":\"" + sTopic + "/set/ForceCharge\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // SOC Trick Enable Switch
  mqttPublish((baseTopic + "/switch/" + nodeId + "_soctrick/config").c_str(),
    (String("{\"name\":\"SOC Trick Enable\",\"unique_id\":\"") + nodeId + "_soctrick\"," +
     "\"state_topic\":\"" + sTopic + "/Param/SOCTrickEnable\"," +
     "\"command_topic\":\"" + sTopic + "/set/SOCTrickEnable\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Request Flags Enable Switch
  mqttPublish((baseTopic + "/switch/" + nodeId + "_requestflags/config").c_str(),
    (String("{\"name\":\"Request Flags Enable\",\"unique_id\":\"") + nodeId + "_requestflags\"," +
     "\"state_topic\":\"" + sTopic + "/Param/RequestFlagsEnable\"," +
     "\"command_topic\":\"" + sTopic + "/set/RequestFlagsEnable\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Smart Charge Switch
  mqttPublish((baseTopic + "/switch/" + nodeId + "_smartcharge/config").c_str(),
    (String("{\"name\":\"Smart Charge\",\"unique_id\":\"") + nodeId + "_smartcharge\"," +
     "\"state_topic\":\"" + sTopic + "/Param/SmartCharge\"," +
     "\"command_topic\":\"" + sTopic + "/set/SmartCharge\"," +
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Charge Voltage Number Control
  mqttPublish((baseTopic + "/number/" + nodeId + "_chargevoltage/config").c_str(),
    (String("{\"name\":\"Charge Voltage\",\"unique_id\":\"") + nodeId + "_chargevoltage\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{{ (value_json.chargevoltage * 0.001) | round(1) }}\"," +
     "\"command_topic\":\"" + sTopic + "/set/ChargeVoltage\"," +
     "\"unit_of_measurement\":\"V\",\"device_class\":\"voltage\"," +
     "\"min\":4.0,\"max\":58.0,\"step\":0.1" +
     deviceJson + "}").c_str(), true);
  yield();
  
  // Charge Current Number Control
  mqttPublish((baseTopic + "/number/" + nodeId + "_chargecurrent/config").c_str(),
    (String("{\"name\":\"Charge Current\",\"unique_id\":\"") + nodeId + "_chargecurrent\"," +
     "\"state_topic\":\"" + sTopic + "/Data\"," +
     "\"value_template\":\"{{ (value_json.chargecurrent * 0.001) | round(1) }}\"," +
     "\"command_topic\":\"" + sTopic + "/set/ChargeCurrent\"," +
     "\"unit_of_measurement\":\"A\",\"device_class\":\"current\"," +
     "\"min\":0.0,\"max\":100.0,\"step\":0.1" +
     deviceJson + "}").c_str(), true);
  yield();
  
  log_i("Home Assistant Discovery published successfully.");
  WS_LOG_I("Home Assistant Discovery published successfully.");
}

void connectToMqtt() {
    if (!mqttEnabled) return;
    log_i("Connecting to MQTT...");
    mqttClient.connect();
}

void onMqttConnect(bool sessionPresent) {
  log_d("Connected to MQTT.");
  WS_LOG_I("MQTT connected to %s", wifiManager.GetMQTTServerIP().c_str());
  Lcd.Data.MQTTConnected.setValue(true);
  mqttClient.setWill((sTopic + "/status").c_str(), 2, true, "offline");
  yield();
  mqttClient.subscribe((sTopic + "/set/#").c_str(), 2);
  yield();
  mqttPublish((sTopic + "/status").c_str(), "online", true);
  yield();
  
  // Publish Home Assistant Discovery
  publishHADiscovery();
  yield();
  
  // Send initial state updates
  sendUpdateMQTTData();
}

void onMqttDisconnect(bool sessionPresent) {
  log_d("Disconnected from MQTT, sessionPresent: %d, Cleaning up pending messages.", sessionPresent);
  Lcd.Data.MQTTConnected.setValue(false);
  for (int i = 0; i < MAX_PENDING_MSGS; i++) {
  if (pending_msgs[i].active) {
    free(pending_msgs[i].payloadbuffer);
    free(pending_msgs[i].topicbuffer);
    pending_msgs[i].payloadbuffer = nullptr;
    pending_msgs[i].topicbuffer = nullptr;
    pending_msgs[i].msg_id = -1;
    pending_msgs[i].active = false;
    }
  }
}

void onMqttSubscribe(uint16_t msg_id) {
  log_d("Subscribe acknowledged. Msg ID: %d", msg_id);
}

void onMqttUnsubscribe(uint16_t msg_id) {
  log_d("Unsubscribe acknowledged. Msg ID: %d", msg_id);
}

void onMqttError(esp_mqtt_error_codes_t error) {
  log_e("MQTT Error: %s, Type: %d, Connect Return Code: %d, ESP Transport Sock Errno: %d",
        esp_err_to_name(error.esp_tls_last_esp_err), error.error_type, error.connect_return_code, error.esp_transport_sock_errno);
  WS_LOG_E("MQTT Error: %s, Type: %d, Connect Return Code: %d, ESP Transport Sock Errno: %d",
        esp_err_to_name(error.esp_tls_last_esp_err), error.error_type, error.connect_return_code, error.esp_transport_sock_errno);  
      }

void onMqttMessage(char* topic, char* payload, int retain, int qos, bool dup) {
   
String _Topic = String(topic);
String message = String(payload);
log_i("MQTT Message: %s, Topic: %s", message.c_str(), _Topic.c_str());

if (_Topic == (wifiManager.GetMQTTTopic() + "/set/DischargeCurrent")) {

    Inverter.SetDischargeCurrent(message.toInt());
    log_d("Discharge current set to: %d", message.toInt());
    WS_LOG_I("Discharge current set to: %d", message.toInt());
  }
  else if (_Topic == (wifiManager.GetMQTTTopic() + "/set/ChargeVoltage")) {
    float voltageV = message.toFloat();
    int voltagemV = (int)round(voltageV * 1000.0);  // Convert V to mV with proper rounding
    if (voltagemV > 0) {
      Inverter.SetChargeVoltage(voltagemV);
      log_d("Charge voltage set to: %.1f V (%d mV)", voltageV, voltagemV);
      WS_LOG_I("Charge voltage set to: %.1f V (%d mV)", voltageV, voltagemV);
    }
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/ChargeCurrent") {
   float currentA = message.toFloat();
   int currentmA = (int)round(currentA * 1000.0);  // Convert A to mA with proper rounding
   Inverter.SetChargeCurrent(currentmA);
   log_d("Charge current set to: %.1f A (%d mA)", currentA, currentmA);
    WS_LOG_I("Charge current set to: %.1f A (%d mA)", currentA, currentmA);
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/ForceCharge") {
    bool forcecharge = (message == "ON") ? true : false;
    Inverter.ForceCharge((message == "ON") ? true : false);
    log_d("Force charge set to: %d", forcecharge);
    WS_LOG_I("Force charge set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/DischargeEnable") {
    Inverter.ManualAllowDischarge((message == "ON") ? true : false);
    log_d("Discharge enable set to: %s", (message == "ON") ? "ON" : "OFF");
    WS_LOG_I("Discharge enable set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/ChargeEnable") {
    Inverter.ManualAllowCharge((message == "ON") ? true : false);
    log_d("Charge enable set to: %s", (message == "ON") ? "ON" : "OFF");
    WS_LOG_I("Charge enable set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/SOCTrickEnable") {
    Inverter.EnableSOCTrick((message == "ON") ? true : false);
    log_d("SOC Trick Enable set to: %s", (message == "ON") ? "ON" : "OFF");
    WS_LOG_I("SOC Trick Enable set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/RequestFlagsEnable") {
    Inverter.EnableRequestFlags((message == "ON") ? true : false);
    log_d("Request Flags Enable set to: %s", (message == "ON") ? "ON" : "OFF");
    WS_LOG_I("Request Flags Enable set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/SmartCharge") {
    Inverter.AutoCharge((message == "ON") ? true : false);
    log_d("Smart Charge set to: %s", (message == "ON") ? "ON" : "OFF");
    WS_LOG_I("Smart Charge set to: %s", (message == "ON") ? "ON" : "OFF");
    // Publish updated state immediately
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/SmartCharge").c_str(), (Inverter.AutoCharge() == true) ? "ON" : "OFF" , true);
  }
  
}

void onMqttPublish(uint16_t msg_id) {
 
  // Free buffer associated with this msg_id
  log_i("MQTT Publish acknowledged. Msg ID: %d", msg_id);
  for (int i = 0; i < MAX_PENDING_MSGS; i++) {
    if (pending_msgs[i].msg_id == msg_id) {
      free(pending_msgs[i].payloadbuffer);
      free(pending_msgs[i].topicbuffer);
      pending_msgs[i].payloadbuffer = nullptr;
      pending_msgs[i].topicbuffer = nullptr;
      pending_msgs[i].msg_id = -1;
      pending_msgs[i].active = false;
      break;
    }
  }

}

// Declare the mutex as a static variable at file scope
static portMUX_TYPE MqttMutex = portMUX_INITIALIZER_UNLOCKED;

void mqttsetup() {

  memset(pending_msgs, 0, sizeof(pending_msgs));
  for (int i = 0; i < MAX_PENDING_MSGS; i++) {
      pending_msgs[i].payloadbuffer = nullptr;
      pending_msgs[i].topicbuffer = nullptr;
      pending_msgs[i].msg_id = -1;
      pending_msgs[i].active = false;
  }

  taskENTER_CRITICAL(&MqttMutex);
    sServer = String(wifiManager.GetMQTTServerIP().c_str());
    sUser = String(wifiManager.GetMQTTUser().c_str());
    sPass = String(wifiManager.GetMQTTPass().c_str());
    sTopic = String(wifiManager.GetMQTTTopic().c_str());
    sTopicData = String((wifiManager.GetMQTTTopic() + wifiManager.GetMQTTParameter()).c_str());
    sClientid = String(wifiManager.GetMQTTClientID().c_str());
    iPort = wifiManager.GetMQTTPort();
    taskEXIT_CRITICAL(&MqttMutex);
    if (sServer.length() == 0 || iPort < 1) {
      log_i("MQTT details not set, not connecting to MQTT.");
      mqttEnabled = false;
      return;
    }
    if(sServer.startsWith("mqtt://") || sServer.startsWith("ws://")) {
      sServer += String(":") + String(iPort);
      mqttClient.setServer(sServer.c_str());
    } else {
      sServer = String("mqtt://") + sServer + String(":") + String(iPort);
      mqttClient.setServer(sServer.c_str());
    }
   
    mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
    log_d("Setting MQTT Server to: %s", sServer.c_str());
   
    mqttClient.setCredentials(sUser.c_str(),sPass.c_str());
    mqttEnabled = true;

    if (sClientid.length() < 2) {
      sClientid = "DiyBatteryBMS_" + String(WiFi.macAddress());
      log_d("MQTT Client ID not set, using default: %s", sClientid.c_str());
    }
    mqttClient.setClientId(sClientid.c_str());
    mqttClient.onPublish(onMqttPublish);
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onSubscribe(onMqttSubscribe);
  //  mqttClient.onUnsubscribe(onMqttUnsubscribe);
    mqttClient.onMessage(onMqttMessage);  


    
}
