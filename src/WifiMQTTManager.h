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
        // millis() when the current outage started, 0 while connected
        unsigned long _wifiDownSince = 0;
        unsigned long _wifiDropCount = 0;   // drops since boot, for the rate

        /* The station watchdog. WiFi.isConnected() answers about the radio
           association and nothing above it, so a board whose IP stack has been
           starved of internal RAM stays "connected" with an address and serves
           nothing at all, indefinitely, and the reconnect path above never
           fires. That is the fault this catches.

           _lastServiceOkMs is the last time something proved the stack works -
           see NoteServiceOk(). Zero means it never has, which is also how a
           board with no broker configured stays exempt: nothing is expected of
           it, so nothing is diagnosed. */
        unsigned long _lastServiceOkMs = 0;
        unsigned long _lastReinitMs    = 0;
        unsigned long _reinitBackoffMs = 0;   // 0 = no re-init done yet
        unsigned long _reinitCount     = 0;
        bool ReinitDue(unsigned long now) const;
        void ReinitWiFi(const char* why);
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
        /* Same name as the hostname, so one device answers to one word
           everywhere: mDNS, the access point, and the client id a broker logs.
           A broker drops the older session when two clients claim one id, so a
           name that reads like the device and not like the project is worth
           having when a second board turns up on the same broker.
           Existing devices keep whatever is in NVS. */
        String _mqttClientID = "diy-battery-bms";
        /* Not renamed with the rest. This is the root of every topic the device
           publishes, so changing it moves every reading to a new address:
           dashboards stop updating, Home Assistant discovers a second copy of
           the device, and automations quietly stop firing. It only bites new
           installs, but there is nothing to gain in exchange - a topic root is
           typed once and then only ever read by machines. */
        String _mqttTopic = "DIY-BATTERY";
        String _mqttParameter = "/Param";

    public:
        WifiMQTTManagerClass() {
            _provEnable = false;
        }
        bool begin();
        void loop();
        /* Something just proved the IP stack actually works end to end. An MQTT
           session coming up is the signal that matters: it is persistent,
           unattended, and it fails when the stack is starved, which a browser
           nobody has open cannot tell us.

           Calling this also arms the watchdog. Until it has been called once,
           the board is not expected to be serving anything and is never
           re-initialised for failing to - which is what keeps a device with no
           broker configured out of this entirely. */
        void NoteServiceOk();
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