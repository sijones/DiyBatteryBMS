//#ifdef WIFIMANAGER
//#pragma once
#include <Arduino.h>
#include "mEEPROM.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>


class WifiMQTTManagerClass {
    private:
        DNSServer _dnsserver;
        mEEPROM m_pref;
        bool _provEnable;
        bool _needConfig;
        bool _wifiOK;

        bool _wifiEnabled = false;
        bool _dnsStarted = false;
        String _wifiSSID = "";
        String _wifiPass = "";
        String _mqttServer = "";
        String _wifiHostName = "DIY-SMARTBMS";
        String _mqttUser = "";
        String _mqttPass = "";
        uint16_t _mqttPort = 1883;
        String _mqttClientID = "smartbms-dev";

    public:
        WifiMQTTManagerClass() {
            _provEnable = false;
        }
        bool begin();
        void loop();
        bool isWiFiConnected();
        bool isMqttConnected();
        bool isWifiSetup();
        bool WifiConnect();
        bool WifiConfig();
        bool MQTTConnect();
        bool WifiDisconnect();
        bool MQTTDisconnect();
        void setClock();
        bool mqttEnabled = false;
        bool mqttConnected = false;
        bool mqttInit = false;
        String GetIPAddr();
        wifi_mode_t GetMode();
        WiFiClient wifiClient;

        String _mqttTopic = "SMARTBMS-dev";
        String _mqttParameter = "/Param";
        String GetWifiSSID();
        String GetWifiPass();
        String GetWifiHostName();
        String GetMQTTUser();
        String GetMQTTPass();
        String GetMQTTServerIP();
        String GetMQTTClientID();
        String GetMQTTTopic();
        String GetMQTTParameter();
        uint16_t GetMQTTPort();

        void SetWifiSSID(String SSID);
        void SetWifiPass(String Pass);
        void SetWifiHostName(String HostName);
        void SetMQTTUser(String User);
        void SetMQTTPass(String Pass);
        void SetMQTTServerIP(String ServerIP);
        void SetMQTTClientID(String ClientID);
        void SetMQTTTopic(String Topic);
        void SetMQTTParameter(String Parameter);
        void SetMQTTPort(uint16_t Port);
        
};

//#endif