
#include <WiFi.h>
#include <Arduino.h>
#include "mEEPROM.h"

extern "C" {
	#include "freertos/FreeRTOS.h"
	#include "freertos/timers.h"
}
#include <AsyncMqttClient.h>

AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;
//mEEPROM pref;
bool mqttEnabled = false;
String sUser;
String sPass;
String sServer;
String sSubscribe;
String sTopic;
String sTopicData;
char buffer[10];

bool mqttPublish(String topic, String payload, bool retain)
{
  return mqttPublish(topic.c_str(),payload.c_str(),retain);
}

bool mqttPublish(const char* topic, const char* payload, bool retain)
{
  if (mqttClient.connected())
    return mqttClient.publish(topic,0,retain,payload);
  else 
    return false;
    
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
    return false; 
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

  // Send Voltage
  sprintf (buffer, "%u", Inverter.BattVoltage());
  mqttPublish((sTopic + "/V").c_str(),buffer,false);
  // Send Current
  sprintf (buffer, "%i", Inverter.BattCurrentmA());
  mqttPublish((sTopic + "/I").c_str(),buffer,false);
  // Send SOC
  sprintf (buffer, "%i", Inverter.BattSOC());
  mqttPublish((sTopic + "/SOC").c_str(),buffer,false);

  // Publish Json with more details in.
  mqttPublish(sTopicData.c_str(),generateDatatoJSON(false).c_str(),false);
  return true;
}

void connectToMqtt() {
    if (!mqttEnabled) return;
    log_i("Connecting to MQTT...");
    //if (sUser.length()>0)
    log_i("Using User: %s, Password: %s",sUser,sPass);
    //mqttClient.setCredentials(sUser.c_str(),sPass.c_str());
    mqttClient.setCredentials(sUser.c_str(),sPass.c_str());
    mqttClient.connect();
}

void WiFiEvent(WiFiEvent_t event) {
    log_d("[WiFi-event] event: %d\n", event);
    switch(event) {
    case SYSTEM_EVENT_STA_GOT_IP:
        log_d("WiFi connected, IP Address: %s",WiFi.localIP().toString());
        connectToMqtt();
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        log_d("WiFi lost connection");
        xTimerStop(mqttReconnectTimer, 0); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
        break;
    }
}

void onMqttConnect(bool sessionPresent) {
  log_d("Connected to MQTT.");
  Lcd.Data.MQTTConnected.setValue(true);
  sSubscribe = pref.getString(ccMQTTTopic,"");
  if (!sSubscribe.endsWith("/"))
    sSubscribe += "/";

  //uint16_t packetIdSub = mqttClient.subscribe((sSubscribe + "set/#").c_str(), 0);

  mqttClient.subscribe((sSubscribe + "set/ChargeEnable").c_str(),2);
  mqttClient.subscribe((sSubscribe + "set/DischargeEnable").c_str(),2);
  mqttClient.subscribe((sSubscribe + "set/ForceCharge").c_str(),2);
  mqttClient.subscribe((sSubscribe + "set/EnablePYLONTECH").c_str(),2);
  mqttClient.subscribe((sSubscribe + "set/ChargeVoltage").c_str(),2);
  mqttClient.subscribe((sSubscribe + "set/ChargeCurrent").c_str(),2);
  
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {

  switch (reason)
  {
    case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED:
      log_d("Disconnected from MQTT due to TCP Disconnection");
      break;
    case AsyncMqttClientDisconnectReason::ESP8266_NOT_ENOUGH_SPACE:
      log_d("Disconnected from MQTT due to Not Enough Space");
      break;
    case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED:
      log_d("Disconnected from MQTT, due to MQTT Identifier Rejected");
      break;
    case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS:
      log_d("Disconnected from MQTT, due to Malformed Credentials");
      break;
    case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED:
      log_d("Disconnected from MQTT, due to Not Authorised");
      break;
    case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE:
      log_d("Disconnected from MQTT, due to Server Unavailable");
      break;
    case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
      log_d("Disconnected from MQTT, Unacceptable Protocol Version");
      break;
    case AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT:
      log_d("Disconnected from MQTT, due to Bad TLS Fingerprint");
      break;
    default:
      log_d("Disconnected from MQTT for Unknown Reason");
      break;
  }

  Lcd.Data.MQTTConnected.setValue(false);

  if (WiFi.isConnected()) {
      xTimerStart(mqttReconnectTimer, 0);
  }

}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  log_d("Subscribe acknowledged. packetId: %d qos: %d", packetId, qos);
}

void onMqttUnsubscribe(uint16_t packetId) {
  log_d("Unsubscribe acknowledged.");
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  log_d("Message received. Topic: %s, Payload: %s",topic,payload);
  
String sTopic = String(topic);
String message = String(payload,len);

if (sTopic == (wifiManager.GetMQTTTopic() + "/set/DischargeCurrent")) {
    Inverter.SetDischargeCurrent(message.toInt());
  }
  else if (sTopic == (wifiManager.GetMQTTTopic() + "/set/ChargeVoltage")) {
    if (message.toInt() > 0) {
      Inverter.SetChargeVoltage(message.toInt());
    }
  }
  else if (sTopic == wifiManager.GetMQTTTopic() + "/set/ChargeCurrent") {
   Inverter.SetChargeCurrent(message.toInt());
  }
  else if (sTopic == wifiManager.GetMQTTTopic() + "/set/ForceCharge") {
    bool forcecharge = (message == "ON") ? true : false;
    Inverter.ForceCharge((message == "ON") ? true : false);
    log_d("Force charge set to: %d", forcecharge);
   }
  else if (sTopic == wifiManager.GetMQTTTopic() + "/set/DischargeEnable") {
    Inverter.ManualAllowDischarge((message == "ON") ? true : false); 
   }
  else if (sTopic == wifiManager.GetMQTTTopic() + "/set/ChargeEnable") {
    Inverter.ManualAllowCharge((message == "ON") ? true : false); 
   }
  else if (sTopic == wifiManager.GetMQTTTopic() + "/set/EnablePYLONTECH") {
    Inverter.EnablePylonTech((message == "ON") ? true : false); 
   }
  
}

void onMqttPublish(uint16_t packetId) {
  //log_d("Publish acknowledged. packetId: %d", packetId);
}

void mqttsetup() {

    sServer = wifiManager.GetMQTTServerIP();
    sUser = wifiManager.GetMQTTUser();
    sPass = wifiManager.GetMQTTPass();
    sTopic = wifiManager.GetMQTTTopic();
    sTopicData = wifiManager.GetMQTTTopic() + "/Data";
    uint16_t mqttPort = (uint16_t) pref.getUInt16(ccMQTTPort,mqttPort);
    IPAddress _ip;
    bool useIP = false;
    
    mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));

    WiFi.onEvent(WiFiEvent);

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
  //  mqttClient.onSubscribe(onMqttSubscribe);
  //  mqttClient.onUnsubscribe(onMqttUnsubscribe);
    mqttClient.onMessage(onMqttMessage);
  //  mqttClient.onPublish(onMqttPublish);

    if (sServer.length()>0)
    {
        if (_ip.fromString(sServer))
        {
            mqttEnabled = true;
            log_d("Using IP Address method to connect.");
            mqttClient.setServer(_ip,mqttPort);
        }
        else {
            log_d("Using Hostname method to connect.");
            mqttClient.setServer(sServer.c_str(),mqttPort);
        }

    }
    else return;

}
