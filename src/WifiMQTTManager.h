//#ifdef WIFIMANAGER
//#pragma once
#include <Arduino.h>
#include "mEEPROM.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>

class WifiMQTTManagerClass {
    private:
        DNSServer _dnsserver;
        mEEPROM m_pref;
        bool _provEnable;
        bool _needConfig;
        bool _wifiOK;
        bool _wifiEnabled = false;
        bool _dnsStarted = false;
        unsigned long _lastWifiCheckTime = 0;
        unsigned long _wifiReconnectDelay = 10000; // 10 seconds between reconnect attempts
        bool _wifiWasConnected = false;
        String _wifiSSID = "";
        String _wifiPass = "";
        String _mqttServer = "";
        /* Doubles as the mDNS name and, on a board with no credentials yet, the
           SSID of the access point it raises - so it is the first thing a new
           user ever sees this device called. Lower case because that is how it
           appears in a browser bar either way, and it matches the -bms name used
           everywhere else. Only new devices take this: an existing one has its
           name in NVS and keeps it. */
        String _wifiHostName = "diy-battery-bms";
        String _mqttUser = "";
        String _mqttPass = "";
        uint16_t _mqttPort = 1883;
        String _mqttClientID = "diy-battery";
        String _mqttTopic = "DIY-BATTERY";
        String _mqttParameter = "/Param";

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