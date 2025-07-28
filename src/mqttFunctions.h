
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
String sUser;
String sPass;
String sServer;
String sSubscribe;
String sTopic;
String sTopicData;
String sClientid;
uint16_t iPort = 1883; // Default MQTT Port

//char buffer[10];

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
  int msg_id = mqttClient.publish(topicBuffer,1,retain,payloadBuffer,lenPayload+1,true);

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
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/EnablePYLONTECH").c_str(), (Inverter.EnablePylonTech() == true) ? "ON" : "OFF",true);
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/ForceCharge").c_str(), (Inverter.ForceCharge() == true) ? "ON" : "OFF" , true);  
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/DischargeEnable").c_str(), (Inverter.DischargeEnable() == true && Inverter.ManualAllowDischarge()) ? "ON" : "OFF" , true); 
    mqttPublish((wifiManager.GetMQTTTopic() + "/Param/ChargeEnable").c_str(), (Inverter.ChargeEnable() == true && Inverter.ManualAllowCharge()) ? "ON" : "OFF" , true); 
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
  //sprintf (buffer, "%u", Inverter.BattVoltage());
  //mqttPublish((sTopic + "/V").c_str(),buffer,false);
  // Send Current
  //sprintf (buffer, "%i", Inverter.BattCurrentmA());
  //mqttPublish((sTopic + "/I").c_str(),buffer,false);
  // Send SOC
  //sprintf (buffer, "%i", Inverter.BattSOC());
  //mqttPublish((sTopic + "/SOC").c_str(),buffer,false);

  return true;
}

void connectToMqtt() {
    if (!mqttEnabled) return;
    log_i("Connecting to MQTT...");
    mqttClient.connect();
}

void onMqttConnect(bool sessionPresent) {
  log_d("Connected to MQTT.");
  Lcd.Data.MQTTConnected.setValue(true);
  mqttClient.setWill((sTopic + "/status").c_str(), 2, true, "offline");
  yield();
  mqttClient.subscribe((sTopic + "/set/#").c_str(), 2);
  yield();
  mqttPublish((sTopic + "/status").c_str(), "online", true);
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
}

void onMqttMessage(char* topic, char* payload, int retain, int qos, bool dup) {
   
String _Topic = String(topic);
String message = String(payload);
log_i("MQTT Message: %s, Topic: %s", message.c_str(), _Topic.c_str());

if (_Topic == (wifiManager.GetMQTTTopic() + "/set/DischargeCurrent")) {

    Inverter.SetDischargeCurrent(message.toInt());
    log_d("Discharge current set to: %d", message.toInt());
  }
  else if (_Topic == (wifiManager.GetMQTTTopic() + "/set/ChargeVoltage")) {
    if (message.toInt() > 0) {
      Inverter.SetChargeVoltage(message.toInt());
      log_d("Charge voltage set to: %d", message.toInt());
    }
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/ChargeCurrent") {
   Inverter.SetChargeCurrent(message.toInt());
   log_d("Charge current set to: %d", message.toInt());
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/ForceCharge") {
    bool forcecharge = (message == "ON") ? true : false;
    Inverter.ForceCharge((message == "ON") ? true : false);
    log_d("Force charge set to: %d", forcecharge);
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/DischargeEnable") {
    Inverter.ManualAllowDischarge((message == "ON") ? true : false);
    log_d("Discharge enable set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/ChargeEnable") {
    Inverter.ManualAllowCharge((message == "ON") ? true : false);
    log_d("Charge enable set to: %s", (message == "ON") ? "ON" : "OFF");
  }
  else if (_Topic == wifiManager.GetMQTTTopic() + "/set/EnablePYLONTECH") {
    Inverter.EnablePylonTech((message == "ON") ? true : false);
    log_d("Enable PylonTech set to: %s", (message == "ON") ? "ON" : "OFF");
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
