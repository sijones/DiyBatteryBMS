
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

#if !MQTT_ASSUME_CLIENT_COPIES
typedef struct
{
  char *payloadbuffer;
  char *topicbuffer;
  int msg_id;
  uint32_t millis;
  bool active;
} mqtt_msg_t;

static mqtt_msg_t pending_msgs[MAX_PENDING_MSGS];
#endif

bool mqttPublish(String topic, String payload, bool retain)
{
  return mqttPublish(topic.c_str(),payload.c_str(),retain);
}

/* qos 0 for the Home Assistant discovery burst.

   Discovery is ~42 retained config messages sent back to back. At qos 1 every
   one of them is copied into esp-mqtt's outbox and held there until the broker
   sends a PUBACK, which on a heap-starved board is where it fell over:
     E (2070) outbox: outbox_enqueue(53): Memory exhausted
   followed by abort(). Retained messages do not need the delivery guarantee -
   the broker keeps the last one on each topic, so a config that is lost in
   flight is replaced on the next connect anyway, and Home Assistant re-reads
   all of them when it subscribes. Live data keeps qos 1. */
bool mqttPublishQos(const char* topic, const char* payload, bool retain, int qos);

bool mqttPublish(const char* topic, const char* payload, bool retain)
{
  return mqttPublishQos(topic, payload, retain, 1);
}

/* Whether the buffers handed to publish() have to outlive the call is a question
   about the client library, not something to take on trust. This code kept its
   own copies because an earlier client - AsyncMqttClient, still named in the
   commented-out include at the top of this file - genuinely did not copy, and
   getting that wrong is a use-after-free that shows up as corrupted topics under
   load rather than as a clean crash.

   PsychicMqttClient does copy. Not on the strength of the esp-mqtt docs, which
   have been wrong before and describe a library that has been swapped under this
   code once already, but measured on hardware: mqttRunCopyTest() publishes a
   marker from a heap buffer, memsets that buffer and frees it the instant
   publish() returns, and the marker still arrives at the broker intact -

     Copy test RESULT: received 'COPYTEST-abcdefghijklmnop' -> client DOES copy

   which it could not if the mqtt task were still reading our memory. It has to
   copy: enqueue() hands the send to another task, so there is no buffer of ours
   it could rely on.

   Re-run that test after any change of MQTT client or IDF version, and set this
   back to 0 if it ever fails. */
#ifndef MQTT_ASSUME_CLIENT_COPIES
#define MQTT_ASSUME_CLIENT_COPIES 1
#endif

bool mqttPublishQos(const char* topic, const char* payload, bool retain, int qos)
{
  if (!mqttClient.connected()) return false;

#if MQTT_ASSUME_CLIENT_COPIES
  const int msg_id = mqttClient.publish(topic, qos, retain, payload, strlen(payload), true);
  if (msg_id < 0) {
    log_e("MQTT enqueue failed (%d) for %s", msg_id, topic);
    return false;
  }
  return true;
#else
  // Reclaim anything the broker never acknowledged
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
  int msg_id = mqttClient.publish(topicBuffer,qos,retain,payloadBuffer,lenPayload,true);

  if (msg_id < 0) {
      log_e("Failed to enqueue message");
      free(payloadBuffer);
      free(topicBuffer);
      return false;
  }

  /* Note for when qos 0 is switched on: nothing acknowledges a qos 0 publish, so
     onMqttPublish never comes back to release these and they sit until the
     timeout sweep - 42 discovery messages would fill the table long before that.
     Freeing them here instead is only safe if the client really does copy, which
     is what MQTT_ASSUME_CLIENT_COPIES is waiting on. */

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
#endif
}

static char _mqTopicBuf[64];

/*
   Publish what the scheduler is currently doing, so the plan is visible and
   debuggable from Home Assistant rather than only from the device log.
   Retained, so a newly started HA sees the current state immediately.
*/
#ifndef DISABLE_SCHEDULER
void publishScheduleStatus() {
  if (otaInProgress || !mqttClient.connected()) return;
  const char* t = sTopic.c_str();
  time_t now = time(nullptr);
  SchedDecision d = Schedule.evaluate(now, Inverter.BattSOC(), Inverter.ChargeEnable());

  auto pub = [&](const char* suffix, const char* val) {
    snprintf(_mqTopicBuf, sizeof(_mqTopicBuf), "%s/Schedule/%s", t, suffix);
    mqttPublish(_mqTopicBuf, val, true);
  };
  char buf[24];
  pub("Active", d.active ? "ON" : "OFF");
  pub("Source", d.active ? (d.fromMqtt ? "mqtt" : "ui") : "none");
  snprintf(buf, sizeof(buf), "%u", (unsigned)Schedule.mqttCount()); pub("Windows", buf);
  pub("ForceCharge", d.force ? "ON" : "OFF");
  pub("ChargeAllowed", d.charge ? "ON" : "OFF");
  pub("DischargeAllowed", d.discharge ? "ON" : "OFF");
  snprintf(buf, sizeof(buf), "%u", (unsigned)d.targetSOC); pub("TargetSOC", buf);
  time_t nxt = Schedule.nextMqttStart(now);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)nxt); pub("NextStart", buf);
  // What the schedule wants is published above; these say whether it is actually
  // in charge, or whether something outside has taken a lever off it.
  pub("Override", RemoteOverride.Any() ? "ON" : "OFF");
  snprintf(buf, sizeof(buf), "%u", (unsigned)RemoteOverride.SecondsLeft()); pub("OverrideSecs", buf);
}
#else
static inline void publishScheduleStatus() {}
#endif

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
  pub("RequestFullCharge", Inverter.RequestFullCharge() ? "ON" : "OFF");
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
// Sized for the longest discovery payload: scaffolding + name + unique_id + state topic
// + value template + extra attributes + up to 192 bytes of device JSON. The templated
// sensors (SOC override reason, absorption remaining) push past 512.
static char _haBuf[640];
static char _haTopicBuf[128];

void _haPublish(const char* type, const char* id, const char* payload,
                const char* baseTopic, const char* nodeId) {
  snprintf(_haTopicBuf, sizeof(_haTopicBuf), "%s/%s/%s_%s/config", baseTopic, type, nodeId, id);
  /* qos 0. These are retained config messages: the broker keeps the last one
     per topic and Home Assistant re-reads all of them whenever it subscribes,
     so the delivery guarantee buys nothing, while at qos 1 every one of the 42
     sat in esp-mqtt's outbox awaiting a PUBACK - which is where the board ran
     out of memory and aborted. */
  mqttPublishQos(_haTopicBuf, payload, true, 0);
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

/* Discovery is ~42 config messages. Published in one go they overran the
   40-slot pending table and the tail was simply dropped - those entities then
   never appeared in Home Assistant until something else happened to republish
   them. Growing the table would spend RAM permanently on a burst that happens
   once per connection, so the burst is paced instead: one group per pass of the
   main loop, and only once the broker has acknowledged enough of the previous
   group to leave room.

   It has to be paced from the loop rather than by waiting here, because the
   acks that free those slots are delivered on the MQTT task - blocking inside
   onMqttConnect to wait for them would deadlock against the very task that
   would unblock us. */
#define HA_CHUNK_COUNT       5
#define HA_CHUNK_GAP_MS      150
/* Paced against free heap, because that is the resource that actually runs out.
   An earlier version gated on slots free in our own 40-entry table, which said
   nothing about whether esp-mqtt could allocate - the table had room right up to
   the moment the outbox reported "Memory exhausted" and the board aborted. */
#define HA_MIN_FREE_HEAP     24000

static uint8_t  haStep = 0;       // 0 = idle, otherwise the group to send next
static uint32_t haLastStepMs = 0;

// Rebuilt per group rather than held across them: sTopic or the MAC could not
// have changed, but a stale c_str() into a String that has gone is not worth
// the few microseconds saved.
struct HaCtx {
  String nodeIdStr;
  String dataTopicStr;
  char deviceJson[192];
  const char* base;
  const char* node;
  const char* dataTopic;
  const char* st;
};

static void haBuildCtx(HaCtx& c) {
  String deviceId = WiFi.macAddress();
  deviceId.replace(":", "");
  c.nodeIdStr = "diybatterybms_" + deviceId;
  c.dataTopicStr = sTopic + "/Data";
  c.base = "homeassistant";
  c.node = c.nodeIdStr.c_str();
  c.dataTopic = c.dataTopicStr.c_str();
  c.st = sTopic.c_str();
  snprintf(c.deviceJson, sizeof(c.deviceJson),
    ",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"DIY Battery BMS\","
    "\"model\":\"ESP32 BMS\",\"manufacturer\":\"https://github.com/sijones/DiyBatteryBMS\"}",
    c.node);
}

static void haChunk1(HaCtx& c) {
  const char* base = c.base; const char* node = c.node;
  const char* dataTopic = c.dataTopic; const char* deviceJson = c.deviceJson;

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
  haSensor("Charge Current Limit", "chargecurrent", "{{ value_json.chargecurrent | multiply(0.001) | round(0) }}",
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\",\"suggested_display_precision\":0",
    base, node, dataTopic, deviceJson);
  haSensor("Discharge Current Limit", "dischargecurrent", "{{ value_json.dischargecurrent | multiply(0.001) | round(0) }}",
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\",\"suggested_display_precision\":0",
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
}

static void haChunk2(HaCtx& c) {
  const char* base = c.base; const char* node = c.node;
  const char* dataTopic = c.dataTopic; const char* deviceJson = c.deviceJson;

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

  // SOC reported over CAN. Differs from Battery SOC when the firmware is holding 99%,
  // suppressing 100%, or applying the force-charge trick - see the SOC Override sensor.
  haSensor("SOC Sent To Inverter", "reportedsoc", "{{ value_json.reportedsoc }}",
    ",\"unit_of_measurement\":\"%\",\"device_class\":\"battery\",\"state_class\":\"measurement\",\"icon\":\"mdi:battery-sync\"",
    base, node, dataTopic, deviceJson);
  haSensor("SOC Override Reason", "socoverridereason",
    "{% set r = value_json.socoverride | int(0) %}"
    "{{ ['None','SOC trick','Holding 99%','Never 100%'][r] if 0 <= r <= 3 else 'Unknown' }}",
    ",\"entity_category\":\"diagnostic\",\"icon\":\"mdi:battery-alert-variant\"",
    base, node, dataTopic, deviceJson);

  // Absorption progress
  haSensor("Absorption Elapsed", "absorbelapsed", "{{ value_json.absorbelapsed }}",
    ",\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"state_class\":\"measurement\",\"icon\":\"mdi:timer-sand\"",
    base, node, dataTopic, deviceJson);
  haSensor("Absorption Remaining", "absorbremaining",
    "{% set m = value_json.absorbmax | int(0) %}{% set e = value_json.absorbelapsed | int(0) %}"
    "{{ (m - e) if m > 0 and m > e else 0 }}",
    ",\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"state_class\":\"measurement\",\"icon\":\"mdi:timer-outline\"",
    base, node, dataTopic, deviceJson);
  haSensor("Tail Current Held", "tailheld", "{{ value_json.tailheld }}",
    ",\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"state_class\":\"measurement\",\"icon\":\"mdi:timer-check-outline\"",
    base, node, dataTopic, deviceJson);
  haSensor("Tail Current Remaining", "tailremaining",
    "{% set n = value_json.tailneed | int(0) %}{% set h = value_json.tailheld | int(0) %}"
    "{{ (n - h) if n > 0 and n > h else 0 }}",
    ",\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"state_class\":\"measurement\",\"icon\":\"mdi:timer-outline\"",
    base, node, dataTopic, deviceJson);

}

static void haChunk3(HaCtx& c) {
  const char* base = c.base; const char* node = c.node;
  const char* dataTopic = c.dataTopic; const char* deviceJson = c.deviceJson;

  // Binary sensors
  haBinary("Charge Enabled Status", "chargeenabled", "chargeenabled", "", base, node, dataTopic, deviceJson);
  haBinary("Discharge Enabled Status", "dischargeenabled", "dischargeenabled", "", base, node, dataTopic, deviceJson);
  haBinary("Force Charge Status", "forcecharge", "forcecharge", "", base, node, dataTopic, deviceJson);
  haBinary("Smart Charge Status", "smartcharge", "autocharge", "", base, node, dataTopic, deviceJson);
  haBinary("VE.Direct Alarm", "vealarm", "alarmactive", ",\"entity_category\":\"diagnostic\"", base, node, dataTopic, deviceJson);
  haBinary("SOC Override Active", "socoverride", "socoverride", ",\"entity_category\":\"diagnostic\",\"icon\":\"mdi:battery-sync\"", base, node, dataTopic, deviceJson);
  haBinary("Tail Current Active", "tailactive", "tailactive", ",\"icon\":\"mdi:current-dc\"", base, node, dataTopic, deviceJson);

}

static void haChunk4(HaCtx& c) {
  const char* base = c.base; const char* node = c.node;
  const char* st = c.st; const char* deviceJson = c.deviceJson;

  // Switches
  haSwitch("Charge Enable", "charge", "ChargeEnable", base, node, st, deviceJson);
  haSwitch("Discharge Enable", "discharge", "DischargeEnable", base, node, st, deviceJson);
  haSwitch("Force Charge", "forcecharge", "ForceCharge", base, node, st, deviceJson);
  haSwitch("Request Full Charge", "requestfullcharge", "RequestFullCharge", base, node, st, deviceJson);
  haSwitch("SOC Trick Enable", "soctrick", "SOCTrickEnable", base, node, st, deviceJson);
  haSwitch("Request Flags Enable", "requestflags", "RequestFlagsEnable", base, node, st, deviceJson);
  haSwitch("Smart Charge", "smartcharge", "SmartCharge", base, node, st, deviceJson);

}

static void haChunk5(HaCtx& c) {
  const char* base = c.base; const char* node = c.node;
  const char* dataTopic = c.dataTopic; const char* st = c.st;
  const char* deviceJson = c.deviceJson;

  // Number controls
  haNumber("Charge Voltage", "chargevoltage", "{{ (value_json.chargevoltage * 0.001) | round(1) }}", "ChargeVoltage",
    ",\"unit_of_measurement\":\"V\",\"device_class\":\"voltage\",\"min\":4.0,\"max\":58.0,\"step\":0.1",
    base, node, dataTopic, st, deviceJson);
  // Slider max tracks the configured Max Charge Current (mA -> A); fall back to 100 A if unset
  float maxChargeA = Inverter.GetMaxChargeCurrent() * 0.001f;
  if (maxChargeA <= 0.0f) maxChargeA = 100.0f;
  char chargeCurrentExtra[128];
  snprintf(chargeCurrentExtra, sizeof(chargeCurrentExtra),
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"min\":0,\"max\":%.0f,\"step\":1",
    maxChargeA);
  haNumber("Charge Current", "chargecurrent", "{{ (value_json.chargecurrent * 0.001) | round(0) }}", "ChargeCurrent",
    chargeCurrentExtra,
    base, node, dataTopic, st, deviceJson);
  haNumber("Discharge Current", "dischargecurrent", "{{ value_json.dischargecurrent | multiply(0.001) | round(0) }}", "DischargeCurrent",
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"state_class\":\"measurement\",\"suggested_display_precision\":0",
    base, node, dataTopic, st, deviceJson);
  // Float stage. 0 V asks for the automatic target rather than turning float off,
  // so the minimum still has to reach down to 0 rather than stopping at a
  // plausible float voltage.
  haNumber("Float Voltage", "floatvoltage", "{{ (value_json.floatvoltage * 0.001) | round(1) }}", "FloatVoltage",
    ",\"unit_of_measurement\":\"V\",\"device_class\":\"voltage\",\"min\":0,\"max\":58.0,\"step\":0.1,\"entity_category\":\"config\"",
    base, node, dataTopic, st, deviceJson);
  haNumber("Float Current", "floatcurrent", "{{ (value_json.floatcurrent * 0.001) | round(1) }}", "FloatCurrent",
    ",\"unit_of_measurement\":\"A\",\"device_class\":\"current\",\"min\":0,\"max\":50,\"step\":0.5,\"entity_category\":\"config\"",
    base, node, dataTopic, st, deviceJson);
}

/* Arms the sequence. Callers are MQTT and WebSocket callbacks, so this must
   stay cheap - the messages themselves go out from haDiscoveryLoop(). */
void publishHADiscovery() {
  if (!haDiscoveryEnabled) return;
  haStep = 1;
  haLastStepMs = millis() - HA_CHUNK_GAP_MS;   // first group may go immediately
  log_i("Publishing Home Assistant Discovery configs...");
  WS_LOG_I("Publishing Home Assistant Discovery configs...");
}

// Called every pass of the main loop; does nothing unless a sequence is armed.
void haDiscoveryLoop() {
  if (haStep == 0) return;
  if (!haDiscoveryEnabled || !mqttClient.connected() || otaInProgress) {
    haStep = 0;                       // connection or OTA took it away; drop it
    return;
  }
  if ((millis() - haLastStepMs) < HA_CHUNK_GAP_MS) return;
  /* Wait rather than overrun. Holding here is safe: the sequence simply resumes
     on a later pass once the outbox has drained and the heap recovered, and if
     it never does, no discovery is a great deal better than an abort(). */
  if (ESP.getFreeHeap() < HA_MIN_FREE_HEAP) return;

  haLastStepMs = millis();
  HaCtx c;
  haBuildCtx(c);
  switch (haStep) {
    case 1: haChunk1(c); break;
    case 2: haChunk2(c); break;
    case 3: haChunk3(c); break;
    case 4: haChunk4(c); break;
    case 5: haChunk5(c); break;
  }

  if (++haStep > HA_CHUNK_COUNT) {
    haStep = 0;
    log_i("Home Assistant Discovery published successfully.");
    WS_LOG_I("Home Assistant Discovery published successfully.");
  }
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
  
  // Arm Home Assistant Discovery - the main loop feeds it out from here
  publishHADiscovery();
  yield();

  // Send initial state updates. Retained, so Home Assistant picks them up when
  // it subscribes to each entity, whether that happens before or after this.
  sendUpdateMQTTData();
}

void onMqttDisconnect(bool sessionPresent) {
  log_d("Disconnected from MQTT, sessionPresent: %d", sessionPresent);
  WS_LOG_W("MQTT disconnected, scheduling reconnect...");
  Lcd.Data.MQTTConnected.setValue(false);
#if !MQTT_ASSUME_CLIENT_COPIES
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
#endif

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

if (_Topic.endsWith("/set/CopyTest")) {
  // Round trip of the buffer-lifetime test - see mqttRunCopyTest()
  const bool intact = (message == "COPYTEST-abcdefghijklmnop");
  WS_LOG_I("Copy test RESULT: received '%s' -> client %s copy the payload",
           message.c_str(), intact ? "DOES" : "does NOT");
  return;
}

if (false) { }
#ifndef DISABLE_SCHEDULER
  else if (_Topic == (wifiManager.GetMQTTTopic() + "/set/Schedule")) {
    // Retained by the publisher, so the broker replays it on reconnect and the
    // device recovers its plan after a reboot without any flash write.
    String err;
    int n = Schedule.ingest(message.c_str(), err);
    if (n < 0) WS_LOG_W("Schedule rejected: %s", err.c_str());
    else {
      WS_LOG_I("Schedule accepted: %d window(s) from MQTT%s%s",
               n, err.length() ? " - " : "", err.c_str());
    }
    publishScheduleStatus();
  }
#endif
  else if (_Topic == (wifiManager.GetMQTTTopic() + "/set/DischargeCurrent")) {

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
  // The next three latch their lever so the scheduler does not undo them on its
  // next pass. The latch times out - see RemoteOverride.h.
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/ForceCharge") {
    bool forcecharge = (message == "ON") ? true : false;
    Inverter.ForceCharge((message == "ON") ? true : false);
    RemoteOverride.Arm(OV_FORCE);
    log_d("Force charge set to: %d", forcecharge);
    WS_LOG_I("Force charge set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  // Not a scheduler lever - it changes how a charge finishes, not whether one
  // starts - so no override latch here.
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/RequestFullCharge") {
    Inverter.RequestFullCharge((message == "ON") ? true : false);
    log_d("Request full charge set to: %s", (message == "ON") ? "ON" : "OFF");
    WS_LOG_I("Request full charge set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/DischargeEnable") {
    Inverter.ManualAllowDischarge((message == "ON") ? true : false);
    RemoteOverride.Arm(OV_DISCHARGE);
    log_d("Discharge enable set to: %s", (message == "ON") ? "ON" : "OFF");
    WS_LOG_I("Discharge enable set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/ChargeEnable") {
    Inverter.ManualAllowCharge((message == "ON") ? true : false);
    RemoteOverride.Arm(OV_CHARGE);
    log_d("Charge enable set to: %s", (message == "ON") ? "ON" : "OFF");
    WS_LOG_I("Charge enable set to: %s", (message == "ON") ? "ON" : "OFF");
  }
#ifndef DISABLE_SCHEDULER
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/ClearOverride") {
    // Give the schedule control back now instead of waiting out the latch
    RemoteOverride.Clear();
    WS_LOG_I("Remote override cleared, schedule back in control");
    publishScheduleStatus();
  }
#endif
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
  // 0 V turns the float stage off, so unlike the other voltages this one accepts
  // zero rather than rejecting it as an unset value.
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/FloatVoltage") {
    float voltageV = message.toFloat();
    uint32_t voltagemV = (uint32_t)round(voltageV * 1000.0);
    Inverter.SetFloatVoltage((uint16_t) voltagemV);
    pref.putUInt32(ccFloatVoltage, voltagemV);
    log_d("Float voltage set to: %.1f V (%u mV)", voltageV, voltagemV);
    WS_LOG_I("Float voltage set to: %.1f V - holding %u mV%s", voltageV,
             Inverter.ActiveFloatVoltage(),
             !Inverter.FloatEnabled()           ? " (float stage off)"
             : Inverter.FloatUsingAutoVoltage() ? " (automatic)" : "");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/FloatCurrent") {
    float currentA = message.toFloat();
    uint32_t currentmA = (uint32_t)round(currentA * 1000.0);
    Inverter.SetFloatCurrent(currentmA);
    pref.putUInt32(ccFloatCurrent, currentmA);
    log_d("Float current set to: %.1f A (%u mA)", currentA, currentmA);
    WS_LOG_I("Float current set to: %.1f A (%u mA)", currentA, currentmA);
  }

}

void onMqttPublish(uint16_t msg_id) {
  log_d("MQTT Publish acknowledged. Msg ID: %d", msg_id);
#if !MQTT_ASSUME_CLIENT_COPIES
  // Free buffer associated with this msg_id
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
#endif
}

/* Does the client copy what we hand publish(), or must our buffer outlive the
   call? The whole pending_msgs table exists because of that question, and the
   docs are not evidence - the client behind this API has been swapped before.

   Publishes a known marker from a heap buffer, scribbles the buffer the
   instant publish() returns, then frees it. The device is already subscribed to
   its own <topic>/set/#, so whatever the broker actually received comes back to
   onMqttMessage and is logged. Marker intact => the client copied. Marker
   corrupted or missing => it did not, and the copies stay.

   Triggered from the web UI by sending {"mqttcopytest": true}. Costs nothing
   when unused; delete once the answer is recorded in the code. */
static bool mqttCopyTestPending = false;

void mqttRunCopyTest() {
  if (!mqttClient.connected()) { WS_LOG_W("Copy test: MQTT not connected"); return; }

  const char* marker = "COPYTEST-abcdefghijklmnop";
  const size_t len = strlen(marker);

  char* topic = (char*)malloc(96);
  char* payload = (char*)malloc(len + 1);
  if (!topic || !payload) { free(topic); free(payload); WS_LOG_E("Copy test: out of memory"); return; }
  snprintf(topic, 96, "%s/set/CopyTest", sTopic.c_str());
  strcpy(payload, marker);

  // Straight to the client, deliberately bypassing mqttPublish's bookkeeping
  const int msg_id = mqttClient.publish(topic, 0, false, payload, len, true);

  // Poison both buffers before the mqtt task can possibly have sent them
  memset(payload, 'Z', len);
  memset(topic, 'Z', 95); topic[95] = '\0';
  free(payload);
  free(topic);

  WS_LOG_I("Copy test: published id=%d, buffers poisoned and freed. Expect '%s' back.",
           msg_id, marker);
}

// Declare the mutex as a static variable at file scope
static portMUX_TYPE MqttMutex = portMUX_INITIALIZER_UNLOCKED;

void mqttsetup() {

#if !MQTT_ASSUME_CLIENT_COPIES
  memset(pending_msgs, 0, sizeof(pending_msgs));
  for (int i = 0; i < MAX_PENDING_MSGS; i++) {
      pending_msgs[i].payloadbuffer = nullptr;
      pending_msgs[i].topicbuffer = nullptr;
      pending_msgs[i].msg_id = -1;
      pending_msgs[i].active = false;
  }
#endif

  /* Everything that can block is done before the critical section, not inside
     it. taskENTER_CRITICAL disables interrupts and takes a spinlock, so nothing
     in here may wait on anything - and reading NVS takes a mutex. Arduino core
     2.x let that pass; core 3.x calls abort() from lock_acquire_generic when a
     lock is taken with no way to yield, which boot-looped a fresh board on the
     first run. The reads themselves are safe out here: this runs once at
     startup, before anything else touches these values. */
  String battTopic = pref.getString(ccMQTTBattTopic, "");
  String invTopic  = pref.getString(ccMQTTInvTopic, "");
  String server    = String(wifiManager.GetMQTTServerIP().c_str());
  String user      = String(wifiManager.GetMQTTUser().c_str());
  String pass      = String(wifiManager.GetMQTTPass().c_str());
  String topic     = String(wifiManager.GetMQTTTopic().c_str());
  String clientid  = String(wifiManager.GetMQTTClientID().c_str());
  uint16_t port    = wifiManager.GetMQTTPort();

  /* Second gate, after mEEPROM::getString's. These values reach us through the
     WiFi manager's cached copies rather than a fresh NVS read, so a bad one read
     before this check existed - or written by an older build - can still be
     sitting in memory. MQTT 3.1.1 requires well-formed UTF-8 in every string of
     the CONNECT packet, and PsychicMqttClient passes them through untouched, so
     a broker's only answer is to drop the link. Better to stay off the broker
     and say why. */
  auto utf8ok = [](const char* what, const String& v) -> bool {
    if (isValidUTF8(v)) return true;
    log_e("MQTT %s is not valid UTF-8 (%u bytes), refusing to send it", what, (unsigned)v.length());
    WS_LOG_E("MQTT %s is not valid UTF-8, check the setting and re-save it", what);
    return false;
  };

  bool connectOK = true;
  if (!utf8ok("server address", server))  connectOK = false;
  if (!utf8ok("username", user))          connectOK = false;
  if (!utf8ok("password", pass))          connectOK = false;
  if (!utf8ok("base topic", topic))       connectOK = false;
  if (!utf8ok("client ID", clientid))     connectOK = false;
  if (!connectOK) {
    log_e("MQTT settings failed validation, not connecting.");
    WS_LOG_E("MQTT settings failed validation, not connecting");
    mqttEnabled = false;
    return;
  }

  /* The temperature topics are optional subscriptions, so a bad one drops just
     that subscription instead of taking MQTT down with it. */
  if (!utf8ok("battery temperature topic", battTopic)) battTopic = "";
  if (!utf8ok("inverter temperature topic", invTopic)) invTopic = "";

  // Only the handover to the shared copies needs guarding
  taskENTER_CRITICAL(&MqttMutex);
    sServer = server;
    sUser = user;
    sPass = pass;
    sTopic = topic;
    sClientid = clientid;
    iPort = port;
    sMqttBattTopic = battTopic;
    sMqttInvTopic = invTopic;
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
