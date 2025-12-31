
//#ifdef WIFIMANAGER
#include "WifiMQTTManager.h"

bool WifiMQTTManagerClass::begin()
{
    m_pref.begin("network");
    log_d("Attempting to get WiFi/MQTT details from NVS");

    if(m_pref.isKey(ccWifiSSID))
        _wifiSSID = m_pref.getString(ccWifiSSID,_wifiSSID);
    if(m_pref.isKey(ccWifiPass))
        _wifiPass = m_pref.getString(ccWifiPass,_wifiPass);

    log_d("Wifi SSID: %s, Password Length: %i",_wifiSSID,_wifiPass.length());

    if(!m_pref.isKey(ccWifiHostName))
        m_pref.putString(ccWifiHostName, _wifiHostName);
    else
        _wifiHostName = m_pref.getString(ccWifiHostName, _wifiHostName);   
    
    _mqttServer = m_pref.getString(ccMQTTServerIP,_mqttServer);

    if(!m_pref.isKey(ccMQTTClientID))
        m_pref.putString(ccMQTTClientID,_mqttClientID);
    else
        _mqttClientID = m_pref.getString(ccMQTTClientID,_mqttClientID);
    if(!m_pref.isKey(ccMQTTPort))
        m_pref.putUInt16(ccMQTTPort,_mqttPort);
    else
        _mqttPort = m_pref.getUInt16(ccMQTTPort,_mqttPort);
    if(!m_pref.isKey(ccMQTTParam))
        m_pref.putString(ccMQTTParam,_mqttParameter);
    else
        _mqttParameter = m_pref.getString(ccMQTTParam,_mqttParameter);
        
    if(!m_pref.isKey(ccMQTTTopic))
        m_pref.putString(ccMQTTTopic,_mqttTopic);
    else
        _mqttTopic = m_pref.getString(ccMQTTTopic,_mqttTopic);

    if(!m_pref.isKey(ccMQTTUser))
        m_pref.putString(ccMQTTUser,_mqttUser);
    else
        _mqttUser = m_pref.getString(ccMQTTUser,_mqttUser);
    if (!m_pref.isKey(ccMQTTPass))
        m_pref.putString(ccMQTTPass,_mqttPass);
    else
        _mqttPass = m_pref.getString(ccMQTTPass,_mqttPass);
    
    if(_wifiSSID.length() > 2 && _wifiPass.length() > 4) {
        log_d("Connecting to SSID: %s", _wifiSSID);
        WiFi.mode(WIFI_MODE_STA);
        WiFi.begin(_wifiSSID.c_str(),_wifiPass.c_str());

        if (_wifiHostName.length() > 1) {
            log_d("Setting up mDNS Service.");
            MDNS.begin(_wifiHostName.c_str());
            MDNS.addService("http", "tcp", 80);
        }
        WiFi.setAutoReconnect(true);
        _wifiEnabled = true;
        _needConfig = false;
    }
    else {
        log_d("Wifi needs configuring, Starting AP Mode");
        _needConfig = true;
        WiFi.mode(WIFI_MODE_AP);
        // Prevent AP-side modem sleep to reduce idle disconnects
        WiFi.setSleep(false);
        // Explicit AP network config (gateway = AP IP)
        WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
        // Start SoftAP on a non-DFS, common channel (1) to reduce roaming
        WiFi.softAP(_wifiHostName.c_str(), NULL, 1);
        /* Setup the DNS server redirecting all the domains to the apIP */
        delay(50);
        _dnsserver.setErrorReplyCode(DNSReplyCode::NoError);
        if (_dnsserver.start(53,"*", WiFi.softAPIP())) {
            log_d("DNS Server Started.");
            _dnsStarted = true;
        }
        else
            log_d("DNS Server Failed to Start");
        return false;
    }

    if (m_pref.isKey(ccMQTTClientID) && m_pref.isKey(ccMQTTServerIP))
    {
        IPAddress ipaddr;
        
        if (_mqttClientID.length() > 1 && (ipaddr.fromString(_mqttServer) || _mqttServer.length() >= 2) && _mqttPort > 20 && _mqttPort < 65535)
        {
            mqttEnabled = true;
        }
        else {
            log_d("MQTT details stored are not valid.");
            log_d("MQTT Server IP: %s, MQTT Port %d, MQTT Client ID: %s",_mqttServer,_mqttPort,_mqttClientID);
            if(_mqttTopic.length() < 2)
                log_d("MQTT Topic: %s",_mqttTopic);
        } 

    } else 
    {
        log_d("MQTT server not set.");
        log_d("MQTT Server IP: %s, MQTT Port %d, MQTT Client ID: %s",_mqttServer,_mqttPort,_mqttClientID);
    }

    return true;
}

bool WifiMQTTManagerClass::WifiConfig()
{
    return _needConfig;
}

bool WifiMQTTManagerClass::isWiFiConnected()
{
    
    if (WiFi.getMode() == WIFI_MODE_STA)
        return WiFi.isConnected();
    else
        return false;
}


bool WifiMQTTManagerClass::WifiConnect()
{
    return true;
    WifiDisconnect();
    //WiFi.begin();
}

bool WifiMQTTManagerClass::WifiDisconnect()
{
    return true;
    //WiFi.disconnect();
}

bool WifiMQTTManagerClass::MQTTConnect()
{
    return false;
}

void WifiMQTTManagerClass::loop()
{
    if (_dnsStarted)         
      _dnsserver.processNextRequest();
}

String WifiMQTTManagerClass::GetIPAddr()
{

    if (WiFi.getMode() == WIFI_MODE_AP)
        return WiFi.softAPIP().toString();
    else if (WiFi.getMode() == WIFI_MODE_STA)
        return WiFi.localIP().toString();
    else 
        return "?";
}

wifi_mode_t WifiMQTTManagerClass::GetMode()
{
    return WiFi.getMode();
}

String WifiMQTTManagerClass::GetWifiSSID(){ return _wifiSSID; }
String WifiMQTTManagerClass::GetWifiPass(){ return _wifiPass; }
String WifiMQTTManagerClass::GetWifiHostName(){ return _wifiHostName; }
String WifiMQTTManagerClass::GetMQTTUser() { return _mqttUser; }
String WifiMQTTManagerClass::GetMQTTPass() { return _mqttPass; }
String WifiMQTTManagerClass::GetMQTTServerIP(){ return _mqttServer; }
String WifiMQTTManagerClass::GetMQTTClientID(){ return _mqttClientID; }
String WifiMQTTManagerClass::GetMQTTTopic(){ return _mqttTopic; }
String WifiMQTTManagerClass::GetMQTTParameter(){ return _mqttParameter; }
uint16_t WifiMQTTManagerClass::GetMQTTPort(){ return _mqttPort; }

void WifiMQTTManagerClass::SetWifiSSID(String SSID){
    _wifiSSID = SSID;
    //m_pref.begin(PREF_NAME);
    m_pref.putString(ccWifiSSID, _wifiSSID);
    //m_pref.end();
}
void WifiMQTTManagerClass::SetWifiPass(String Pass){
    _wifiPass = Pass;
    //m_pref.begin(PREF_NAME);
    m_pref.putString(ccWifiPass, _wifiPass);
    //m_pref.end();
}
void WifiMQTTManagerClass::SetWifiHostName(String HostName){
    _wifiHostName = HostName;
    //m_pref.begin(PREF_NAME);
     m_pref.putString(ccWifiHostName,_wifiHostName); 
     //m_pref.end();
}
void WifiMQTTManagerClass::SetMQTTUser(String User){
    _mqttUser = User;
    //m_pref.begin(PREF_NAME);
    m_pref.putString(ccMQTTUser, _mqttUser);
    //m_pref.end();
}
void WifiMQTTManagerClass::SetMQTTPass(String Pass){
    _mqttPass = Pass;
    //m_pref.begin(PREF_NAME);
    m_pref.putString(ccMQTTPass, _mqttPass);
    //m_pref.end();
}
void WifiMQTTManagerClass::SetMQTTServerIP(String ServerIP){
    _mqttServer = ServerIP;
    //m_pref.begin(PREF_NAME);
    m_pref.putString(ccMQTTServerIP,_mqttServer);
    //m_pref.end();
}
void WifiMQTTManagerClass::SetMQTTClientID(String ClientID){
    _mqttClientID = ClientID;
    //m_pref.begin(PREF_NAME);
    m_pref.putString(ccMQTTClientID,_mqttClientID);
    //m_pref.end();
}
void WifiMQTTManagerClass::SetMQTTTopic(String Topic){
    _mqttTopic = Topic;
    //m_pref.begin(PREF_NAME);
    m_pref.putString(ccMQTTTopic,_mqttTopic);
    //m_pref.end();
}
void WifiMQTTManagerClass::SetMQTTParameter(String Parameter){
    _mqttParameter = Parameter;
    while (_mqttParameter.endsWith("/"))
    {
        _mqttParameter.remove(_mqttParameter.length()-1,1);
    }
    if (!_mqttParameter.startsWith("/")){
        _mqttParameter = String("/" + _mqttParameter);
    }
    //m_pref.begin(PREF_NAME);
    m_pref.putString(ccMQTTParam,_mqttParameter);
    //m_pref.end();
}
void WifiMQTTManagerClass::SetMQTTPort(uint16_t Port){
        _mqttPort = Port;
        if (_mqttPort >= 20 && _mqttPort <= 65535) {
            //m_pref.begin(PREF_NAME);
            m_pref.putUInt16(ccMQTTPort,_mqttPort);
            //m_pref.end();
        }
}


//#endif