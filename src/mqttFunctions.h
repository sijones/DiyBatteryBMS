
#include <WiFi.h>
#include <Arduino.h>
#include "mEEPROM.h"

extern "C" {
	#include "freertos/FreeRTOS.h"
	#include "freertos/timers.h"
}
//#include <AsyncMqttClient.h>
#include <PsychicMqttClient.h>
#define MAX_PENDING_MSGS 40
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

// MQTT temperature subscription topics (loaded from NVS)
String sMqttBattTopic = "";
String sMqttInvTopic = "";
String sTopic;
String sClientid;
uint16_t iPort = 1883; // Default MQTT Port

char buffer[16];

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

static char _mqTopicBuf[64];

bool sendUpdateMQTTData()
{
  if (otaInProgress) return false;
  if (!mqttClient.connected()) {
    log_e("MQTT not connected, cannot send update data.");
    return false;
  }
  const char* t = sTopic.c_str();
  auto pub = [&](const char* suffix, const char* val) {
    snprintf(_mqTopicBuf, sizeof(_mqTopicBuf), "%s/Param/%s", t, suffix);
    mqttPublish(_mqTopicBuf, val, true);
  };
  pub("SOCTrickEnable", Inverter.EnableSOCTrick() ? "ON" : "OFF");
  pub("RequestFlagsEnable", Inverter.EnableRequestFlags() ? "ON" : "OFF");
  pub("ForceCharge", Inverter.ForceCharge() ? "ON" : "OFF");
  pub("DischargeEnable", (Inverter.DischargeEnable() && Inverter.ManualAllowDischarge()) ? "ON" : "OFF");
  pub("ChargeEnable", (Inverter.ChargeEnable() && Inverter.ManualAllowCharge()) ? "ON" : "OFF");
  pub("SmartCharge", Inverter.AutoCharge() ? "ON" : "OFF");
  return true;
}

bool sendVE2MQTT() {
  if (otaInProgress) return false;
  const char* t = sTopic.c_str();
  auto pub = [&](const char* suffix, const char* val) {
    snprintf(_mqTopicBuf, sizeof(_mqTopicBuf), "%s/%s", t, suffix);
    mqttPublish(_mqTopicBuf, val, false);
  };

  snprintf(_mqTopicBuf, sizeof(_mqTopicBuf), "%s/Data", t);
  mqttPublish(_mqTopicBuf, generateDatatoJSON(false).c_str(), false);

  sprintf(buffer, "%u", Inverter.BattVoltage());   pub("V", buffer);
  sprintf(buffer, "%i", Inverter.BattCurrentmA());  pub("I", buffer);
  sprintf(buffer, "%i", Inverter.BattSOC());         pub("SOC", buffer);
  sprintf(buffer, "%ld", Inverter.BattPower());      pub("P", buffer);
  sprintf(buffer, "%d", Inverter.BattTemp());        pub("T", buffer);
  sprintf(buffer, "%ld", Inverter.TimeToGo());       pub("TTG", buffer);
  pub("Alarm", Inverter.AlarmActive() ? "ON" : "OFF");
  pub("AR", Inverter.AlarmReason().c_str());
  if (Inverter.MqttInverterTemp() != -127) {
    sprintf(buffer, "%d", Inverter.MqttInverterTemp()); pub("InverterTemp", buffer);
  }
  if (Inverter.MqttBattTemp() != -127) {
    sprintf(buffer, "%d", Inverter.MqttBattTemp()); pub("MQTTBattTemp", buffer);
  }
  sprintf(buffer, "%u", FAN_PWM); pub("FanPWM", buffer);
  snprintf(_mqTopicBuf, sizeof(_mqTopicBuf), "%s/Param/ChargePhase", t);
  mqttPublish(_mqTopicBuf, Inverter.GetChargePhaseName(), false);

  return true;
}

// HA Discovery helpers — build JSON with snprintf to avoid String heap churn
static char _haBuf[512];
static char _haTopicBuf[128];

void _haPublish(const char* type, const char* id, const char* payload,
                const char* baseTopic, const char* nodeId) {
  snprintf(_haTopicBuf, sizeof(_haTopicBuf), "%s/%s/%s_%s/config", baseTopic, type, nodeId, id);
  mqttPublish(_haTopicBuf, payload, true);
  yield();
}

void haSensor(const char* name, const char* id, const char* valueTpl, const char* extra,
              const char* baseTopic, const char* nodeId, const char* stateTopic, const char* deviceJson) {
  snprintf(_haBuf, sizeof(_haBuf),
    "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"%s\"%s%s}",
    name, nodeId, id, stateTopic, valueTpl, extra, deviceJson);
  _haPublish("sensor", id, _haBuf, baseTopic, nodeId);
}

void haBinary(const char* name, const char* id, const char* jsonField, const char* extra,
              const char* baseTopic, const char* nodeId, const char* stateTopic, const char* deviceJson) {
  snprintf(_haBuf, sizeof(_haBuf),
    "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{%% if value_json.%s %%}ON{%% else %%}OFF{%% endif %%}\","
    "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"%s%s}",
    name, nodeId, id, stateTopic, jsonField, extra, deviceJson);
  _haPublish("binary_sensor", id, _haBuf, baseTopic, nodeId);
}

void haSwitch(const char* name, const char* id, const char* paramName,
              const char* baseTopic, const char* nodeId, const char* sTopic, const char* deviceJson) {
  snprintf(_haBuf, sizeof(_haBuf),
    "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
    "\"state_topic\":\"%s/Param/%s\","
    "\"command_topic\":\"%s/set/%s\","
    "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"%s}",
    name, nodeId, id, sTopic, paramName, sTopic, paramName, deviceJson);
  _haPublish("switch", id, _haBuf, baseTopic, nodeId);
}

void haNumber(const char* name, const char* id, const char* valueTpl, const char* cmdSuffix,
              const char* extra, const char* baseTopic, const char* nodeId,
              const char* stateTopic, const char* sTopic, const char* deviceJson) {
  snprintf(_haBuf, sizeof(_haBuf),
    "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"%s\","
    "\"command_topic\":\"%s/set/%s\"%s%s}",
    name, nodeId, id, stateTopic, valueTpl, sTopic, cmdSuffix, extra, deviceJson);
  _haPublish("number", id, _haBuf, baseTopic, nodeId);
}

void publishHADiscovery() {
  if (!haDiscoveryEnabled || !mqttClient.connected()) return;

  String deviceId = WiFi.macAddress();
  deviceId.replace(":", "");
  String nodeIdStr = "diybatterybms_" + deviceId;
  const char* base = "homeassistant";
  const char* node = nodeIdStr.c_str();
  String dataTopicStr = sTopic + "/Data";
  const char* dataTopic = dataTopicStr.c_str();
  const char* st = sTopic.c_str();

  char deviceJson[192];
  snprintf(deviceJson, sizeof(deviceJson),
    ",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"DIY Battery BMS\","
    "\"model\":\"ESP32 BMS\",\"manufacturer\":\"https://github.com/sijones/DiyBatteryBMS\"}",
    node);

  log_i("Publishing Home Assistant Discovery configs...");
  WS_LOG_I("Publishing Home Assistant Discovery configs...");

  // Sensors
  haSensor("Battery SOC", "soc", "{{ value_json.battsoc }}",
    ",\"unit_of_measurement\":\"%\",\"device_class\":\"battery\",\"state_class\":\"measurement\"",
    base, node, dataTopic, deviceJson);
  haSensor("Battery Voltage", "voltage", "{{ value_json.battvoltage | multiply(0.01) | round(1) }}",
    ",\"unit_of_measurement\":\"V\",\"device_class\":\"voltage\",\"state_class\":\"measurement\",\"suggested_display_precision\":1",
    base, node, dataTopic, deviceJson);
  haSensor("Battery Current", "current", "{{ value_json.battcurrent | multiply(0.1) | round(1) }}",
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\",\"suggested_display_precision\":1",
    base, node, dataTopic, deviceJson);
  haSensor("Battery Temperature", "temperature", "{{ value_json.batttemp }}",
    ",\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\",\"state_class\":\"measurement\"",
    base, node, dataTopic, deviceJson);
  haSensor("Charge Current Limit", "chargecurrent", "{{ value_json.chargecurrent | multiply(0.001) | round(1) }}",
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\"",
    base, node, dataTopic, deviceJson);
  haSensor("Discharge Current Limit", "dischargecurrent", "{{ value_json.dischargecurrent | multiply(0.001) | round(1) }}",
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\",\"suggested_display_precision\":1",
    base, node, dataTopic, deviceJson);
  haSensor("Charge Adjust", "chargeadjust", "{{ value_json.chargeadjust | multiply(0.001) | round(1) }}",
    ",\"state_class\":\"measurement\",\"suggested_display_precision\":1",
    base, node, dataTopic, deviceJson);
  haSensor("Charge Phase", "chargephase", "{{ value_json.chargephase }}",
    ",\"icon\":\"mdi:battery-charging\"",
    base, node, dataTopic, deviceJson);
  haSensor("Battery Power", "power", "{{ value_json.battpower }}",
    ",\"unit_of_measurement\":\"W\",\"device_class\":\"power\",\"state_class\":\"measurement\"",
    base, node, dataTopic, deviceJson);
  haSensor("Time To Go", "timetogo", "{{ value_json.timetogo }}",
    ",\"unit_of_measurement\":\"min\",\"device_class\":\"duration\",\"state_class\":\"measurement\"",
    base, node, dataTopic, deviceJson);
  haSensor("Free Heap", "freeheap", "{{ value_json.freeheap }}",
    ",\"unit_of_measurement\":\"B\",\"entity_category\":\"diagnostic\",\"state_class\":\"measurement\"",
    base, node, dataTopic, deviceJson);
  haSensor("VE.Direct Alarm Reason", "vealarmmessage", "{{ value_json.alarmreason }}",
    ",\"entity_category\":\"diagnostic\"",
    base, node, dataTopic, deviceJson);
  haSensor("Device Model", "devicemodel", "{{ value_json.modelstring }}",
    ",\"entity_category\":\"diagnostic\"",
    base, node, dataTopic, deviceJson);
  haSensor("Device Firmware", "devicefirmware", "{{ value_json.fwversion }}",
    ",\"entity_category\":\"diagnostic\"",
    base, node, dataTopic, deviceJson);
  haSensor("Device Serial Number", "deviceserialnumber", "{{ value_json.serialnumber }}",
    ",\"entity_category\":\"diagnostic\"",
    base, node, dataTopic, deviceJson);
  haSensor("Inverter Temperature", "invertertemp", "{{ value_json.mqttinvertertemp if value_json.mqttinvertertemp != -127 else None }}",
    ",\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\",\"state_class\":\"measurement\"",
    base, node, dataTopic, deviceJson);
  haSensor("Fan PWM", "fanpwm", "{{ value_json.fanpwm }}",
    ",\"unit_of_measurement\":\"%\",\"icon\":\"mdi:fan\",\"state_class\":\"measurement\"",
    base, node, dataTopic, deviceJson);

  // Binary sensors
  haBinary("Charge Enabled Status", "chargeenabled", "chargeenabled", "", base, node, dataTopic, deviceJson);
  haBinary("Discharge Enabled Status", "dischargeenabled", "dischargeenabled", "", base, node, dataTopic, deviceJson);
  haBinary("Force Charge Status", "forcecharge", "forcecharge", "", base, node, dataTopic, deviceJson);
  haBinary("Smart Charge Status", "smartcharge", "autocharge", "", base, node, dataTopic, deviceJson);
  haBinary("VE.Direct Alarm", "vealarm", "alarmactive", ",\"entity_category\":\"diagnostic\"", base, node, dataTopic, deviceJson);

  // Switches
  haSwitch("Charge Enable", "charge", "ChargeEnable", base, node, st, deviceJson);
  haSwitch("Discharge Enable", "discharge", "DischargeEnable", base, node, st, deviceJson);
  haSwitch("Force Charge", "forcecharge", "ForceCharge", base, node, st, deviceJson);
  haSwitch("SOC Trick Enable", "soctrick", "SOCTrickEnable", base, node, st, deviceJson);
  haSwitch("Request Flags Enable", "requestflags", "RequestFlagsEnable", base, node, st, deviceJson);
  haSwitch("Smart Charge", "smartcharge", "SmartCharge", base, node, st, deviceJson);

  // Number controls
  haNumber("Charge Voltage", "chargevoltage", "{{ (value_json.chargevoltage * 0.001) | round(1) }}", "ChargeVoltage",
    ",\"unit_of_measurement\":\"V\",\"device_class\":\"voltage\",\"min\":4.0,\"max\":58.0,\"step\":0.1",
    base, node, dataTopic, st, deviceJson);
  haNumber("Charge Current", "chargecurrent", "{{ (value_json.chargecurrent * 0.001) | round(1) }}", "ChargeCurrent",
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"min\":0.0,\"max\":100.0,\"step\":0.1",
    base, node, dataTopic, st, deviceJson);
  haNumber("Discharge Current", "dischargecurrent", "{{ value_json.dischargecurrent | multiply(0.001) | round(1) }}", "DischargeCurrent",
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\",\"suggested_display_precision\":1",
    base, node, dataTopic, st, deviceJson);

  log_i("Home Assistant Discovery published successfully.");
  WS_LOG_I("Home Assistant Discovery published successfully.");
}

void mqttReconnectTimerCallback(TimerHandle_t xTimer) {
    if (!mqttEnabled) return;
    
    // Only attempt reconnect if WiFi is connected
    if (!wifiManager.isWiFiConnected()) {
        log_w("Cannot reconnect to MQTT: WiFi not connected");
        WS_LOG_W("Cannot reconnect to MQTT: WiFi not connected");
        return;
    }
    
    if (!mqttClient.connected()) {
        log_i("MQTT reconnect timer triggered, attempting connection...");
        WS_LOG_I("MQTT reconnect timer triggered, attempting connection...");
        mqttClient.connect();
    }
}

void connectToMqtt() {
    if (!mqttEnabled) return;
    log_i("Connecting to MQTT...");
    mqttClient.connect();
}

// Re-subscribe to external temperature topics (call after topic/source changes)
void mqttResubscribeTemp() {
    if (!mqttEnabled || !mqttClient.connected()) {
        WS_LOG_W("MQTT not connected, temp subscriptions will apply on next connect");
        return;
    }
    if (Inverter.BattTempSource() == 1 && sMqttBattTopic.length() > 0) {
        mqttClient.subscribe(sMqttBattTopic.c_str(), 1);
        WS_LOG_I("MQTT subscribed to battery temp: %s", sMqttBattTopic.c_str());
    }
    if (Inverter.FanTempSource() == 1 && sMqttInvTopic.length() > 0) {
        mqttClient.subscribe(sMqttInvTopic.c_str(), 1);
        WS_LOG_I("MQTT subscribed to inverter temp: %s", sMqttInvTopic.c_str());
    }
}

void onMqttConnect(bool sessionPresent) {
  log_d("Connected to MQTT.");
  WS_LOG_I("MQTT connected to %s", wifiManager.GetMQTTServerIP().c_str());
  Lcd.Data.MQTTConnected.setValue(true);
  mqttClient.setWill((sTopic + "/status").c_str(), 2, true, "offline");
  yield();
  mqttClient.subscribe((sTopic + "/set/#").c_str(), 2);
  yield();
  // Subscribe to external MQTT temperature topics if configured
  if (Inverter.BattTempSource() == 1 && sMqttBattTopic.length() > 0) {
    mqttClient.subscribe(sMqttBattTopic.c_str(), 1);
    WS_LOG_I("MQTT subscribed to battery temp: %s", sMqttBattTopic.c_str());
    yield();
  } else if (Inverter.BattTempSource() == 1) {
    WS_LOG_W("Battery temp source is MQTT but no topic configured");
  }
  if (Inverter.FanTempSource() == 1 && sMqttInvTopic.length() > 0) {
    mqttClient.subscribe(sMqttInvTopic.c_str(), 1);
    WS_LOG_I("MQTT subscribed to inverter temp: %s", sMqttInvTopic.c_str());
    yield();
  } else if (Inverter.FanTempSource() == 1) {
    WS_LOG_W("Fan temp source is MQTT but no topic configured");
  }
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
  WS_LOG_W("MQTT disconnected, scheduling reconnect...");
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
  
  // Schedule reconnection attempt after delay
  if (mqttEnabled && mqttReconnectTimer != NULL) {
    log_i("Scheduling MQTT reconnect in 10 seconds...");
    WS_LOG_I("Scheduling MQTT reconnect in 10 seconds...");
    xTimerChangePeriod(mqttReconnectTimer, pdMS_TO_TICKS(10000), pdMS_TO_TICKS(100));
    xTimerStart(mqttReconnectTimer, pdMS_TO_TICKS(100));
  }
}

void onMqttSubscribe(uint16_t msg_id) {
  log_d("Subscribe acknowledged. Msg ID: %d", msg_id);
  WS_LOG_I("MQTT subscribe acknowledged (msg %u)", msg_id);
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

// Handle external MQTT temperature subscriptions
if (sMqttBattTopic.length() > 0 && _Topic == sMqttBattTopic) {
    int16_t temp = (int16_t)round(message.toFloat());
    int16_t prev = Inverter.MqttBattTemp();
    Inverter.MqttBattTemp(temp);
    if (Inverter.BattTempSource() == 1) {
        taskENTER_CRITICAL(&(Inverter.CANMutex));
        Inverter.BattTemp(temp);
        taskEXIT_CRITICAL(&(Inverter.CANMutex));
    }
    if (temp != prev) WS_LOG_I("MQTT Battery Temp: %d C", temp);
    return;
}
else if (sMqttInvTopic.length() > 0 && _Topic == sMqttInvTopic) {
    int16_t temp = (int16_t)round(message.toFloat());
    int16_t prev = Inverter.MqttInverterTemp();
    Inverter.MqttInverterTemp(temp);
    if (temp != prev) WS_LOG_I("MQTT Inverter Temp: %d C", temp);
    return;
}

if (_Topic == (wifiManager.GetMQTTTopic() + "/set/DischargeCurrent")) {

    Inverter.SetDischargeCurrent(message.toInt());
    log_d("Discharge current set to: %d", message.toInt());
    WS_LOG_I("Discharge current set to: %d", message.toInt());
  }
  else if (_Topic == (wifiManager.GetMQTTTopic() + "/set/MaxDischargeCurrent")) {
    float currentA = message.toFloat();
    int currentmA = (int)round(currentA * 1000.0);  // Convert A to mA
    if (currentmA > 0) {
      Inverter.SetMaxDischargeCurrent(currentmA);
      log_d("Max discharge current set (runtime) to: %.1f A (%d mA)", currentA, currentmA);
      WS_LOG_I("Max discharge current set (runtime) to: %.1f A (%d mA)", currentA, currentmA);
    }
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
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/TailCurrent") {
    float currentA = message.toFloat();
    uint32_t currentmA = (uint32_t)round(currentA * 1000.0);
    Inverter.SetTailCurrentmA(currentmA);
    pref.putUInt32(ccTailCurrent, currentmA);
    log_d("Tail current set to: %.1f A (%u mA)", currentA, currentmA);
    WS_LOG_I("Tail current set to: %.1f A (%u mA)", currentA, currentmA);
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/RechargeSOC") {
    uint8_t soc = (uint8_t)message.toInt();
    Inverter.SetRechargeSOC(soc);
    pref.putUInt8(ccRechargeSOC, soc);
    log_d("Recharge SOC set to: %u%%", soc);
    WS_LOG_I("Recharge SOC set to: %u%%", soc);
  }

}

void onMqttPublish(uint16_t msg_id) {
 
  // Free buffer associated with this msg_id
  log_d("MQTT Publish acknowledged. Msg ID: %d", msg_id);
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
    sClientid = String(wifiManager.GetMQTTClientID().c_str());
    iPort = wifiManager.GetMQTTPort();
    // Load external MQTT temperature subscription topics
    sMqttBattTopic = pref.getString(ccMQTTBattTopic, "");
    sMqttInvTopic = pref.getString(ccMQTTInvTopic, "");
    taskEXIT_CRITICAL(&MqttMutex);
    log_i("MQTT temp topics: batt='%s' inv='%s'", sMqttBattTopic.c_str(), sMqttInvTopic.c_str());
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
    
    // Create timer for MQTT reconnection (10 second intervals)
    if (mqttReconnectTimer == NULL) {
      mqttReconnectTimer = xTimerCreate("mqttReconnectTimer", pdMS_TO_TICKS(10000), pdFALSE, (void*)0, mqttReconnectTimerCallback);
      if (mqttReconnectTimer == NULL) {
        log_e("Failed to create MQTT reconnect timer");
      } else {
        log_d("MQTT reconnect timer created successfully");
      }
    }
}
