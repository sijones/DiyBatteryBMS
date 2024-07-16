#pragma once

#include <ArduinoJson.h>
#include <WiFi.h>
//#include <AsyncElegantOTA.h>

void setClock() {
  
  log_d("Setting Clock");
  configTime(0, 0, "0.uk.pool.ntp.org", "1.uk.pool.ntp.org");  // UTC
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    now = time(nullptr);
  }
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  log_d("NTP time %s", asctime(&timeinfo));
}

String generateDatatoJSON(bool All)
{
  JsonDocument doc;

  //StaticJsonDocument<750> doc;
  // If ALL is true generate a json with all data
  if (All){
    doc["BMS"] = All;
    doc["chargevoltage"] = Inverter.GetChargeVoltage();
    doc["dischargevoltage"] = Inverter.GetDischargeVoltage();
    doc["maxchargecurrent"] = Inverter.GetMaxChargeCurrent();
    doc["maxdischargecurrent"] = Inverter.GetMaxDischargeCurrent();
    doc["lowsoclimit"] = Inverter.GetLowSOCLimit();
    doc["highsoclimit"] = Inverter.GetHighSOCLimit();
    doc["batterycapacity"] = Inverter.GetBatteryCapacity();
    doc["victrondata"] = Lcd.Data.VEData.getValue();
    doc["canbusinterfaceup"] = Inverter.CanBusAvailable;
    doc["canbusdata"] = Inverter.CanBusDataOK;
    doc["mqttconnected"] = Lcd.Data.MQTTConnected.getValue();
    doc["mqttclientid"] = wifiManager.GetMQTTClientID();
    doc["mqttserverip"] = wifiManager.GetMQTTServerIP();
    doc["mqttport"] = wifiManager.GetMQTTPort();
    doc["victronrxpin"] = pref.getUInt(ccVictronRX,VEDIRECT_RX);
    doc["victrontxpin"] = pref.getUInt(ccVictronTX,VEDIRECT_TX);
    doc["canbusenabled"] = Inverter.CANBusEnabled();
    doc["canbuscspin"] = pref.getUInt(ccCanCSPin,CAN_BUS_CS_PIN);
    doc["pylontechenabled"] = Inverter.EnablePylonTech();
    doc["wifissid"] = wifiManager.GetWifiSSID();
    doc["wifipass"] = wifiManager.GetWifiPass();
    doc["wifihostname"] = wifiManager.GetWifiHostName();
    doc["mqttuser"] = wifiManager.GetMQTTUser();
    doc["mqttpass"] = wifiManager.GetMQTTPass();
    doc["mqttclientid"] = wifiManager.GetMQTTClientID();
    doc["mqttport"] = wifiManager.GetMQTTPort();
    doc["mqtttopic"] = wifiManager.GetMQTTTopic();
    doc["mqttparameter"] = wifiManager.GetMQTTParameter();
    doc["mqttserverip"] = wifiManager.GetMQTTServerIP();
    doc["velooptime"] = VE_LOOP_TIME;
    doc["slowchargesoc1"] = Inverter.GetSlowChargeSOCLimit(1);
    doc["slowchargesoc2"] = Inverter.GetSlowChargeSOCLimit(2);
    doc["slowchargesoc1div"] = Inverter.GetSlowChargeDivider(1);
    doc["slowchargesoc2div"] = Inverter.GetSlowChargeDivider(2);
    doc["lcdenabled"] = pref.getBool(ccLcdEnabled,false);
  }

  doc["RealTime"] = true;
  doc["battsoc"] = Inverter.BattSOC();
  doc["battvoltage"] = Inverter.BattVoltage();
  doc["battcurrent"] = Inverter.BattCurrentmA();
  doc["chargeenabled"] = (Inverter.ChargeEnable() && Inverter.ManualAllowCharge()) ? true : false;
  doc["dischargeenabled"] = (Inverter.DischargeEnable() && Inverter.ManualAllowDischarge()) ? true : false;
  doc["forcecharge"] = Inverter.ForceCharge();
  doc["chargecurrent"] = Inverter.GetChargeCurrent();
  doc["dischargecurrent"] = Inverter.GetDischargeCurrent();
  doc["totalheap"] = ESP.getHeapSize();
  doc["freeheap"] = ESP.getFreeHeap();
  //
  String outputJson;
  //int b =serializeJson(doc, out);
  int b = serializeJson(doc, outputJson);
  return outputJson;

}

void notifyWSClients(bool sendalldata = true) {
  if(ws.count()>0 && ws.availableForWriteAll())
    ws.textAll(generateDatatoJSON(sendalldata));
}

const char * GetWSDataJson(String data, String value)
{
  for (auto x : value)
    {
      if (!isDigit(x) || x == '.' || x == '-' )
        return String("{\"" + data +"\":\"" + value + "\"}").c_str();
    }
    // if we get here all charaters was digits i.e. number
  return String("{\"" + data +"\":" + value + "}").c_str();
}

void handleWSRequest(AsyncWebSocketClient * wsclient,const char * data, int len){

  JsonDocument doc;
  // Check if it's a GET request
  if (strncmp(data,"Get",(int)3)==0) {
    if (strncmp(data,"GetAll()",len)==0)
      notifyWSClients();
    else if (strncmp(data,"GetChargeVoltage",len)==0)
      wsclient->printf(GetWSDataJson((String) "chargevoltage",(String) Inverter.GetChargeVoltage()));
    else if (strncmp(data,"GetDischargeVoltage",len)==0)
      wsclient->printf(GetWSDataJson((String) "dischargevoltage",(String) Inverter.GetDischargeVoltage()));
    else if (strncmp(data,"GetChargeCurrent",len)==0)
      wsclient->printf(GetWSDataJson((String) "chargecurrent",(String) Inverter.GetChargeCurrent()));
    else if (strncmp(data,"GetDischargeCurrent",len)==0)
      wsclient->printf(GetWSDataJson((String) "dischargecurrent",(String) Inverter.GetDischargeCurrent()));
    else if (strncmp(data,"GetMaxChargeCurrent",len)==0)
      wsclient->printf(GetWSDataJson((String) "maxchargecurrent",(String) Inverter.GetMaxChargeCurrent()));
    else if (strncmp(data,"GetMaxDischargeCurrent",len)==0)
      wsclient->printf(GetWSDataJson((String) "maxdischargecurrent",(String) Inverter.GetMaxDischargeCurrent()));
    else if (strncmp(data,"GetSOC()",len)==0)
      wsclient->printf(GetWSDataJson((String) "battsoc",(String) Inverter.BattSOC()));
    else if (strncmp(data,"GetBattCurrent()",len)==0)
      wsclient->printf(GetWSDataJson((String) "battcurrent",(String) Inverter.BattCurrentmA()));
    else if (strncmp(data,"GetBattVoltage()",len)==0)
      wsclient->printf(GetWSDataJson((String) "battvoltage",(String) Inverter.BattVoltage()));
    else if (strncmp(data,"GetChargeEnabled()",len)==0)
      wsclient->printf(GetWSDataJson((String) "chargeenabled",(String) Inverter.ChargeEnable()));
    else if (strncmp(data,"GetDischargeEnabled()",len)==0)
      wsclient->printf(GetWSDataJson((String) "dischargeenabled",(String) Inverter.DischargeEnable()));
    else if (strncmp(data,"GetCANCSPin()",len)==0)
      wsclient->printf(GetWSDataJson((String) "canbuscspin",(String) pref.getUInt(ccCanCSPin,0)));
    else if (strncmp(data,"GetVictronRXPin()",len)==0)
      wsclient->printf(GetWSDataJson((String) "victronrxpin",(String) pref.getUInt(ccVictronRX,0)));
    else if (strncmp(data,"GetVictronTXPin()",len)==0)
      wsclient->printf(GetWSDataJson((String) "victrontxpin",(String) pref.getUInt(ccVictronTX,0)));
    else if (strncmp(data,"GetPylontechEnabled()",len)==0)
      wsclient->printf(GetWSDataJson((String) "pylontechenabled",(String) pref.getUInt(ccPylonTech,0)));
    else 
      wsclient->printf("{\"ERROR\" : \"Unknown Get Request\"}");
    }
    else {
      //if (strncmp(data,"Set",(int)3)==0)  
        // Handle Set Commands
        bool handled = false;
        deserializeJson(doc,data);
       // pref.begin(PREF_NAME);
      if (doc.containsKey("chargevoltage")) {
        Inverter.SetChargeVoltage((uint32_t) doc["chargevoltage"]); 
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("dischargevoltage")) {
        Inverter.SetDischargeVoltage((uint32_t) doc["dischargevoltage"]); 
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("maxchargecurrent")) {
        Inverter.SetMaxChargeCurrent((uint32_t) doc["maxchargecurrent"]); 
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("maxdischargecurrent")) {
        Inverter.SetMaxDischargeCurrent((uint32_t) doc["maxdischargecurrent"]); 
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("chargecurrent")) {
        Inverter.SetChargeCurrent((uint32_t) doc["chargecurrent"]); 
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("dischargecurrent")) {
        Inverter.SetDischargeCurrent((uint32_t) doc["dischargecurrent"]); 
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("batterycapacity")) {
        Inverter.SetBattCapacity((uint32_t) doc["batterycapacity"]);
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("chargeenabled")) {
        Inverter.ChargeEnable((bool) doc["chargeenabled"]); 
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("dischargeenabled")) {
        Inverter.DischargeEnable((bool) doc["dischargeenabled"]);
        handled = true;
        notifyWSClients(); }
      // Low SOC OFF
      if (doc.containsKey("lowsoclimit")) {
        Inverter.SetLowSOCLimit((uint8_t) doc["lowsoclimit"]);
        handled = true;
        notifyWSClients(); }
      // High SOC Limit
      if (doc.containsKey("highsoclimit")) {
        Inverter.SetHighSOCLimit((uint8_t) doc["highsoclimit"]);
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("canbusenabled")) {
        pref.putBool(ccCANBusEnabled, doc["canbusenabled"]);
        Inverter.CANBusEnabled(doc["canbusenabled"]);
        handled = true;
        notifyWSClients(); }

      if (doc.containsKey("lcdenabled")) {
        pref.putBool(ccLcdEnabled, doc["lcdenabled"]);
        if(doc["lcdenabled"])
          Lcd.Enable();
          else
          Lcd.Disable();
        handled = true;
        notifyWSClients(); }
        
      if (doc.containsKey("canbuscspin")) {
        pref.putUInt(ccCanCSPin, doc["canbuscspin"]);
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("victronrxpin")) {
        pref.putUInt(ccVictronRX, doc["victronrxpin"]);
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("victrontxpin")) {
        pref.putUInt(ccVictronTX, doc["victrontxpin"]);
        handled = true;
        notifyWSClients(); }
      if (doc.containsKey("wifissid")) {
        String value = doc["wifissid"];
        handled = true;
        wifiManager.SetWifiSSID(value);
        notifyWSClients();
      }
      if (doc.containsKey("wifipass")) {
        String value = doc["wifipass"];
        handled = true;
        wifiManager.SetWifiPass(value);
      }
      if (doc.containsKey("mqttserverip")) {
        String value = doc["mqttserverip"];
        handled = true;
        wifiManager.SetMQTTServerIP(value);
        notifyWSClients();
      }
      if (doc.containsKey("mqttuser")) {
        String value = doc["mqttuser"];
        handled = true;
        wifiManager.SetMQTTUser(value);
        notifyWSClients();
      }
      if (doc.containsKey("mqttpass")) {
        String value = doc["mqttpass"];
        handled = true;
        wifiManager.SetMQTTPass(value);
      //  notifyWSClients();
      }
      if (doc.containsKey("mqtttopic")) {
        String value = doc["mqtttopic"];
        handled = true;
        wifiManager.SetMQTTTopic(value);
        notifyWSClients();
      }
      if (doc.containsKey("mqttparameter")) {
        String value = doc["mqttparameter"];
        handled = true;
        wifiManager.SetMQTTParameter(value);
        notifyWSClients();
      }
      if (doc.containsKey("mqttclientid")) {
        String value = doc["mqttclientid"];
        handled = true;
        wifiManager.SetMQTTClientID(value);
        notifyWSClients();
      }
      if (doc.containsKey("mqttport")) {
        uint16_t value = doc["mqttport"];
        handled = true;
        wifiManager.SetMQTTPort(value);
        notifyWSClients();
      }
      if (doc.containsKey("wifihostname")) {
        String value = doc["wifihostname"];
        handled = true;
        wifiManager.SetWifiHostName(value);
        notifyWSClients();
      }

      if (doc.containsKey("slowchargesoc1")) {
        uint8_t value = doc["slowchargesoc1"];
        handled = true;
        Inverter.SetSlowChargeSOCLimit(1,value);
        notifyWSClients();
      }
      if (doc.containsKey("slowchargesoc1div")) {
        uint8_t value = doc["slowchargesoc1div"];
        handled = true;
        Inverter.SetSlowChargeDivider(1,value);
        notifyWSClients();
      }
      if (doc.containsKey("slowchargesoc2")) {
        uint8_t value = doc["slowchargesoc2"];
        handled = true;
        Inverter.SetSlowChargeSOCLimit(2,value);
        notifyWSClients();
      }
      if (doc.containsKey("slowchargesoc2div")) {
        uint8_t value = doc["slowchargesoc2div"];
        handled = true;
        Inverter.SetSlowChargeDivider(2,value);
        notifyWSClients();
      }
      if (doc.containsKey("pylontechenabled")) {
        boolean value = doc["pylontechenabled"];
        handled = true;
        pref.putBool(ccPylonTech, value);
        Inverter.EnablePylonTech(value);
        notifyWSClients();
      }
      if (doc.containsKey("velooptime")) {
        uint8_t value = doc["velooptime"];
        handled = true;
        pref.putUInt(ccVELOOPTIME,VE_LOOP_TIME);
        VE_LOOP_TIME = value;
        notifyWSClients();
      }
      
      if (doc.containsKey("reboot")) {
        if(doc["reboot"])
        {
        //  ws.textAll("{ \"Message\" : \"Rebooting now\" }");
        //  delay(25);
          ws.closeAll();
          delay(25);
          handled = true;
          Lcd.ClearScreen();
          ESP.restart();
        }
        else 
        {
          handled = true;
          wsclient->printf("{ \"Message\" : \"To reboot send value true. i.e. {\"reboot\":true } \"}");
        }
      }    
      if (doc.containsKey("saveall")){
        if(doc["saveall"]){
          pref.putUInt(ccChargeVolt,Inverter.GetChargeVoltage());
          pref.putUInt(ccDischargeVolt,Inverter.GetDischargeVoltage());
          pref.putUInt(ccChargeCurrent,Inverter.GetMaxChargeCurrent());
          pref.putUInt(ccDischargeCurrent,Inverter.GetMaxDischargeCurrent());
          pref.putUInt(ccBattCapacity,Inverter.GetBatteryCapacity());
          pref.putUInt(ccLowSOCLimit,Inverter.GetLowSOCLimit());
          pref.putUInt(ccHighSOCLimit,Inverter.GetHighSOCLimit());
          pref.putUInt(ccSlowSOCCharge1,Inverter.GetSlowChargeSOCLimit(1));
          pref.putUInt(ccSlowSOCCharge2,Inverter.GetSlowChargeSOCLimit(2));
          pref.putUInt(ccSlowSOCDivider1,Inverter.GetSlowChargeDivider(1));
          pref.putUInt(ccSlowSOCDivider2,Inverter.GetSlowChargeDivider(2));
          handled = true;
        }
      }

      if (doc.containsKey("eraseall")){
        if(doc["eraseall"]){
          pref.clear();
          handled = true;
          ESP.restart();
        }
      }
      
      if (!handled)
        wsclient->printf("{ \"Message\" : \"ERROR: Unknown Request\" }");
    }   

}

// Web Socket Handler
void onEvent(AsyncWebSocket * wsserver, AsyncWebSocketClient * wsclient, AwsEventType type, void * arg, uint8_t *data, size_t len){
  if(type == WS_EVT_CONNECT){
    //client connected
    log_i("ws[%s][%u] connected\n", wsserver->url(), wsclient->id());
    //wsclient->printf("Your Client %u :)", wsclient->id());
    wsclient->ping();
  } else if(type == WS_EVT_DISCONNECT){
    //client disconnected
    log_i("ws[%s][%u] disconnect: %u\n", wsserver->url(), wsclient->id());
  } else if(type == WS_EVT_ERROR){
    //error was received from the other end
    log_d("ws[%s][%u] error(%u): %s\n", wsserver->url(), wsclient->id(), *((uint16_t*)arg), (char*)data);
  } else if(type == WS_EVT_PONG){
    //pong message was received (in response to a ping request maybe)
    log_i("ws[%s][%u] pong[%u]: %s\n", wsserver->url(), wsclient->id(), len, (len)?(char*)data:"");
    log_i("Sending All Data to All WS Clients");
    notifyWSClients();
  } else if(type == WS_EVT_DATA){
    //data packet
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    if(info->final && info->index == 0 && info->len == len){
      //the whole message is in a single frame and we got all of it's data
      log_d("ws[%s][%u] %s-message[%llu]: ", wsserver->url(), wsclient->id(), (info->opcode == WS_TEXT)?"text":"binary", info->len);
      if(info->opcode == WS_TEXT){
        data[len] = 0;
        log_d("%s\n", (char*)data);
        handleWSRequest(wsclient, (char *)data, info->len);
      } 
    } 
  }
}

void StartCriticalWebService()
{
  log_d("Configuring Web Services.");
  //server.rewrite("/", "/index.html");
  
  // Serve the file "/www/index-ap.htm" in AP, and the file "/www/index.htm" on STA
  
  if (wifiManager.GetMode() == WIFI_MODE_AP) {
    server.rewrite("/", "/index-ap.htm");
     // replay to all requests with same HTML
    server.onNotFound([](AsyncWebServerRequest *request){
      request->redirect("/index-ap.htm");
    });
    log_d("Redirecting root to index-ap.htm"); }
  else {
    server.rewrite("/", "/index.htm");
    log_d("Redirecting root to index.htm"); 
  }
 /* 
 server.on("/mobile/status.php", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("/index-ap.htm");
  });
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("/index-ap.htm");
  }); 
  */   

  AsyncElegantOTA.begin(&server);

  //server.rewrite("/index.htm", "/index-ap.htm").setFilter(ON_AP_FILTER);
  server.serveStatic("/", LittleFS, "/");

  // Web Socket handler
  ws.onEvent(onEvent);
  server.addHandler(&ws);
  // Scan network URL call
  server.on("/scan", HTTP_POST | HTTP_GET, [](AsyncWebServerRequest *request)
  {
    String json = "[";
    int n = WiFi.scanComplete();
    if(n == -2)
    {
      log_d("Starting Network Scan");
      WiFi.scanNetworks(true);
    } 
    else if(n)
    {
      int i = 0;
      for (i = 0; i < n; ++i)
      {
        if(i) json += ",";
        json += "{";
        json += "\"rssi\":"+String(WiFi.RSSI(i));
        json += ",\"ssid\":\""+WiFi.SSID(i)+"\"";
        json += ",\"bssid\":\""+WiFi.BSSIDstr(i)+"\"";
        json += ",\"channel\":"+String(WiFi.channel(i));
        json += ",\"secure\":"+String(WiFi.encryptionType(i));
        json += "}";
      }
      log_d("Network scan returning %d results",i);
      WiFi.scanDelete();
      if(WiFi.scanComplete() == -2)
      {
        WiFi.scanNetworks(true);
      }
    }
    json += "]";
    request->send(200, "application/json", json);
    json = String();
  });
}


// This task is scheduled by setup function, 
// once Wifi is connected it starts Web Services
void taskStartWebServices(void * pointer)
{
  //while (!client.isWifiConnected())
  while (!WiFi.isConnected())
  {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  
  setClock();
  log_d("Starting Web Services");
  
  if (Lcd.Data.LittleFSMounted.getValue()){
   /* server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
    }); */
    StartCriticalWebService();

    server.on("/BMS/ChargeVoltage", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", String(Inverter.GetChargeVoltage()).c_str());
    });
    server.on("/BMS/DischargeVoltage", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", String(Inverter.GetDischargeVoltage()).c_str());
    });
    server.on("/BMS/ChargeCurrent", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", String(Inverter.GetChargeCurrent()).c_str());
    });
    server.on("/BMS/DischargeCurrent", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/plain", String(Inverter.GetDischargeCurrent()).c_str());
    });
    
    Lcd.Data.WebServerState.setValue(true);
  }
  server.begin();

  vTaskDelete( NULL );
}
  
