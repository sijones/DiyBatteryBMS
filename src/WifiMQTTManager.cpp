
//#ifdef WIFIMANAGER
#include "WifiMQTTManager.h"
/* WiFi coming and going is the one class of event this file used to keep to
   itself. log_w and log_i reach neither the web log nor syslog, and at
   CORE_DEBUG_LEVEL=1 they do not reach the serial cable either - so a board
   that spent the night dropping and re-associating looked, in every log anyone
   can actually collect, exactly like a board that had been up all night. */
#include "WebLog.h"
#include <esp_heap_caps.h>   // internal-RAM free, for the station watchdog below

bool WifiMQTTManagerClass::begin()
{
    m_pref.begin("network");
    log_d("Attempting to get WiFi/MQTT details from NVS");

    /* getStringRaw, not getString: WiFi.begin() has to send back the exact bytes
       the access point broadcasts, and an SSID is not required to be UTF-8. See
       the note on getStringRaw in mEEPROM.h. The passphrase follows the SSID for
       the same reason - it is a key, not a sentence. */
    if(m_pref.isKey(ccWifiSSID))
        _wifiSSID = m_pref.getStringRaw(ccWifiSSID,_wifiSSID);
    if(m_pref.isKey(ccWifiPass))
        _wifiPass = m_pref.getStringRaw(ccWifiPass,_wifiPass);

    /* .c_str() on every one of these. Passing a String straight to a %s is
       undefined behaviour, and core 3.x's String is a union with an inline SSO
       buffer, so it silently prints correctly for <= 11 characters and prints
       the bytes of a heap pointer for anything longer. A 12-character SSID
       coming out as line noise reads exactly like corrupted NVS. */
    log_d("Wifi SSID: %s, Password Length: %i",_wifiSSID.c_str(),_wifiPass.length());

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
        log_d("Connecting to SSID: %s", _wifiSSID.c_str());
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
        _wifiWasConnected = false;
        _lastWifiCheckTime = millis();
    }
    else {
        log_d("Wifi needs configuring, Starting AP Mode");
        _needConfig = true;
        WiFi.mode(WIFI_MODE_AP);
        // Prevent AP-side modem sleep to reduce idle disconnects
        WiFi.setSleep(false);

        /* softAP() before softAPConfig(), which is the opposite of the order
           this used. Arduino core 2.x created the AP interface early enough for
           softAPConfig() to land first; core 3.x creates it inside softAP(), so
           configuring first silently did nothing and could leave the AP not
           started at all - no SSID to be seen anywhere.

           Start SoftAP on a non-DFS, common channel (1) to reduce roaming.
           max_connection=4, ssid_hidden=false. */
        const bool apUp = WiFi.softAP(_wifiHostName.c_str(), NULL, 1, false, 4);
        if (!apUp) {
          // Logged at error level on purpose: this is the only way back into a
          // device with no WiFi credentials, so it failing must never be quiet.
          log_e("SoftAP '%s' FAILED to start - no way to configure this device",
                _wifiHostName.c_str());
        }

        // Explicit AP network config (gateway = AP IP), now that it exists
        if (!WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0)))
          log_e("SoftAP IP config failed");

        if (apUp)
          log_e("SoftAP '%s' up on %s", _wifiHostName.c_str(), WiFi.softAPIP().toString().c_str());
        
        // Configure AP for better stability during scans
        // Lower DTIM period (2) means clients wake more frequently to check for buffered traffic
        esp_wifi_set_ps(WIFI_PS_NONE); // Disable power save completely in AP mode
        
        /* Setup the DNS server redirecting all the domains to the apIP */
        delay(50);
        _dnsserver.setErrorReplyCode(DNSReplyCode::NoError);
        if (_dnsserver.start(53,"*", WiFi.softAPIP())) {
            log_d("DNS Server Started.");
            _dnsStarted = true;
        }
        else
            log_d("DNS Server Failed to Start");
        
        // Auto-scan networks so results are ready when web UI connects
        log_d("Starting automatic network scan for AP mode");
        WiFi.scanNetworks(true);
        
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
            log_d("MQTT Server IP: %s, MQTT Port %d, MQTT Client ID: %s",_mqttServer.c_str(),_mqttPort,_mqttClientID.c_str());
            if(_mqttTopic.length() < 2)
                log_d("MQTT Topic: %s",_mqttTopic.c_str());
        } 

    } else 
    {
        log_d("MQTT server not set.");
        log_d("MQTT Server IP: %s, MQTT Port %d, MQTT Client ID: %s",_mqttServer.c_str(),_mqttPort,_mqttClientID.c_str());
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
   // WifiDisconnect();
   // WiFi.begin();
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

/* How long a fault has to persist before the station is cycled.

   Not associating is unambiguous, so it escalates soonest: reconnect() has been
   asked every ten seconds throughout and has plainly not worked.

   "Associated but serving nothing" needs more care, because a broker that is
   simply away looks identical from here. Internal RAM decides which reading is
   more likely: the failure this watchdog exists for is caused by that pool
   running out, so when it is low the short threshold applies, and when it is
   healthy the stall is much more likely to be the broker's own outage and the
   long one does. Either way the ladder below keeps it from thrashing. */
#define WIFI_ASSOC_FAIL_MS    180000UL   // 3 min not associating
#define WIFI_STALL_STARVED_MS 300000UL   // 5 min stalled with internal RAM low
#define WIFI_STALL_HEALTHY_MS 1800000UL  // 30 min stalled with plenty of it
// Below this much internal RAM, a stall is taken to be the stack and not the broker
#define WIFI_STALL_HEAP_FLOOR  40000UL
// The ladder: each re-init that does not fix it waits twice as long as the last
#define WIFI_REINIT_WAIT_MS   300000UL   // 5 min
#define WIFI_REINIT_WAIT_MAX  1800000UL  // ...doubling to 30

void WifiMQTTManagerClass::NoteServiceOk()
{
    unsigned long now = millis();
    _lastServiceOkMs = now ? now : 1;    // 0 is the "never" marker
    /* A working session clears the ladder. The next fault starts from the short
       wait again rather than inheriting a backoff earned by a problem that has
       since been fixed. */
    _lastReinitMs    = 0;
    _reinitBackoffMs = 0;
}

bool WifiMQTTManagerClass::ReinitDue(unsigned long now) const
{
    if (!_lastReinitMs) return true;     // none done yet
    return (unsigned long)(now - _lastReinitMs) >= _reinitBackoffMs;
}

/* Take the station down and bring it back up.

   Deliberately more than WiFi.reconnect(), which re-associates using the driver
   state already in place - no use when that state is the problem. Going through
   WIFI_OFF releases the driver's buffers and the netif with them, which is the
   point on a board that got here by running out of the internal RAM they came
   from.

   Touches nothing but the station: CAN, the charge logic and the shunt all keep
   running through it, so the cost of being wrong is a few seconds of network
   and never a charging interruption. Credentials are left alone - disconnect()
   is asked to power the radio down, not to erase the stored AP. */
void WifiMQTTManagerClass::ReinitWiFi(const char* why)
{
    unsigned long now = millis();
    _lastReinitMs = now ? now : 1;
    _reinitBackoffMs = _reinitBackoffMs ? (_reinitBackoffMs * 2) : WIFI_REINIT_WAIT_MS;
    if (_reinitBackoffMs > WIFI_REINIT_WAIT_MAX) _reinitBackoffMs = WIFI_REINIT_WAIT_MAX;
    _reinitCount++;

    WS_LOG_E("WiFi re-init #%lu: %s (internal RAM %u B free) - cycling the station",
             (unsigned long)_reinitCount, why,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    WiFi.disconnect(true);      // true = power the radio down; AP config is kept
    delay(100);                 // rare enough to afford, and the mode change wants it
    WiFi.mode(WIFI_STA);
    if (_wifiHostName.length()) WiFi.setHostname(_wifiHostName.c_str());
    WiFi.begin(_wifiSSID.c_str(), _wifiPass.c_str());

    /* Restart both clocks. Without this the caller's next pass sees the same
       expired timers and fires again immediately, and the station never gets
       the seconds it needs to associate. */
    _lastWifiCheckTime = now;
    _wifiDownSince     = now ? now : 1;
    _lastServiceOkMs   = _lastServiceOkMs ? (now ? now : 1) : 0;
}

void WifiMQTTManagerClass::loop()
{
    if (_dnsStarted)         
      _dnsserver.processNextRequest();
    
    // Handle WiFi reconnection in STA mode
    if (_wifiEnabled && WiFi.getMode() == WIFI_MODE_STA) {
        bool isConnected = WiFi.isConnected();
        unsigned long now = millis();
        
        // Detect connection loss and log it
        if (!isConnected && _wifiWasConnected) {
            WS_LOG_W("WiFi disconnected, attempting reconnection...");
            _wifiWasConnected = false;
            _lastWifiCheckTime = now;
            _wifiDropCount++;
            _wifiDownSince = now ? now : 1;   // 0 is the "not down" marker
        }

        // Track successful connections
        if (isConnected && !_wifiWasConnected) {
            /* Only a drop that actually happened is called a reconnect.
               _wifiWasConnected starts false, so the first association after
               boot arrives here too, and reporting that as "reconnected after
               3s (drop #0)" invented an outage in the one log someone would
               read to find out whether there had been any. _wifiDownSince is
               set when the link drops and by a re-init, and is zero otherwise,
               which is exactly the distinction wanted. The ordinary first
               connection is already announced with its address elsewhere.

               How long it was away and how many times it has happened since
               boot: a single drop is weather, the same minute repeating all
               night is a fault, and only the count tells them apart after the
               event. */
            if (_wifiDownSince)
                WS_LOG_I("WiFi reconnected after %lus (drop #%lu since boot)",
                         (unsigned long)((now - _wifiDownSince) / 1000UL),
                         (unsigned long)_wifiDropCount);
            _wifiWasConnected = true;
            _lastWifiCheckTime = now;
            _wifiDownSince = 0;
        }

        if (!isConnected) {
            /* Escalate once reconnect() has had long enough to prove it is not
               going to work. It re-associates from driver state that is already
               in place, so when that state is what is broken it can be asked
               forever without effect - which is the shape of the reports. */
            if (_wifiDownSince &&
                (unsigned long)(now - _wifiDownSince) >= WIFI_ASSOC_FAIL_MS &&
                ReinitDue(now)) {
                ReinitWiFi("not associating");
            }
            // Attempt reconnect with backoff timing
            else if ((now - _lastWifiCheckTime) >= _wifiReconnectDelay) {
                /* Every attempt, with how long this outage has run. A reconnect
                   that is being asked for and refused every ten seconds is a very
                   different fault from one that was never attempted, and the two
                   were previously indistinguishable - both were silent. */
                WS_LOG_W("WiFi reconnect attempt, down %lus",
                         (unsigned long)((now - _wifiDownSince) / 1000UL));
                WiFi.reconnect();
                _lastWifiCheckTime = now;
            }
        }
        /* Associated - and this is the case nothing used to examine at all. The
           board holds its address and answers nobody, which no amount of
           isConnected() polling will ever reveal. Only armed once service has
           worked at least once; see NoteServiceOk(). */
        else if (_lastServiceOkMs) {
            const uint32_t internalFree =
                (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            const unsigned long stallLimit =
                (internalFree < WIFI_STALL_HEAP_FLOOR) ? WIFI_STALL_STARVED_MS
                                                       : WIFI_STALL_HEALTHY_MS;
            if ((unsigned long)(now - _lastServiceOkMs) >= stallLimit && ReinitDue(now))
                ReinitWiFi(internalFree < WIFI_STALL_HEAP_FLOOR
                           ? "associated but serving nothing, internal RAM low"
                           : "associated but serving nothing");
        }
    }
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
    m_pref.putString(ccMQTTParam,_mqttParameter);
}
void WifiMQTTManagerClass::SetMQTTPort(uint16_t Port){
        _mqttPort = Port;
        if (_mqttPort >= 20 && _mqttPort <= 65535) {
            m_pref.putUInt16(ccMQTTPort,_mqttPort);
        }
}


//#endif