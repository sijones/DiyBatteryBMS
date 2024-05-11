#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>


struct boolData {
    bool _changed = false;
    bool _currentValue = false;
    bool _oldValue = false;
    bool getValue() {return _currentValue;}
    void setValue(bool Value) { if(_currentValue != Value) {_oldValue = _currentValue; _currentValue = Value; _changed = true;} }
    bool hasChanged() { if (_changed) {_changed = false; return true; } else return false; }
};

struct stringData {
    bool _changed = false;
    String _currentValue;
    String _oldValue;
    String getValue() {return _currentValue;}
    void setValue(String Value) { if(_currentValue != Value) {_oldValue = _currentValue; _currentValue = Value; _changed = true;} }
    bool hasChanged() { if (_changed) {_changed = false; return true; } else return false; }
};

struct uint8Data {
    bool _changed = false;
    uint8_t _currentValue = 0;
    uint8_t _oldValue = 0;
    uint8_t getValue() {return _currentValue;}
    void setValue(uint8_t Value) { if(_currentValue != Value) {_oldValue = _currentValue; _currentValue = Value; _changed = true;} }
    bool hasChanged() { if (_changed) {_changed = false; return true; } else return false; }
};
struct uint16Data {
    bool _changed = false;
    uint16_t _currentValue = 0;
    uint16_t _oldValue = 0;
    uint16_t getValue() {return _currentValue;}
    void setValue(uint16_t Value) { if(_currentValue != Value) {_oldValue = _currentValue; _currentValue = Value; _changed = true;} }
    bool hasChanged() { if (_changed) {_changed = false; return true; } else return false; }
};

struct uint32Data {
    bool _changed = false;
    uint32_t _currentValue = 0;
    uint32_t _oldValue = 0;
    uint32_t getValue() {return _currentValue;}
    void setValue(uint16_t Value) { if(_currentValue != Value) {_oldValue = _currentValue; _currentValue = Value; _changed = true;} }
    bool hasChanged() { if (_changed) {_changed = false; return true; } else return false; }
};
struct int16Data {
    bool _changed = false;
    int16_t _currentValue = 0;
    int16_t _oldValue = 0;
    int16_t getValue() {return _currentValue;}
    void setValue(int16_t Value) { if(_currentValue != Value) {_oldValue = _currentValue; _currentValue = Value; _changed = true;} }
    bool hasChanged() { if (_changed) {_changed = false; return true; } else return false; }
};

struct int32Data {
    bool _changed = false;
    int32_t _currentValue = 0;
    int32_t _oldValue = 0;
    int32_t getValue() {return _currentValue;}
    void setValue(int16_t Value) { if(_currentValue != Value) {_oldValue = _currentValue; _currentValue = Value; _changed = true;} }
    bool hasChanged() { if (_changed) {_changed = false; return true; } else return false; }
};



// Class to store the real time display data
// it converts it on call for the right format to display
class DisplayData
{
public:
    float _roundUInt32(uint32_t f) { return round(f * 10.0) / 10.0; }
    float _roundFloat(float f) { return round(f * 10.0) / 10.0; }
    float _roundInt(int32_t f) { return round(f * 10.0) / 10.0; }

    boolData VEData;
    String GetVEData(){ return (VEData.getValue()) ? String("OK") : String("Failed"); }
    boolData CANInit;
    String GetCANInit() { return (CANInit.getValue()) ? String("OK") : String("Failed");}
    boolData CANBusData;
    String GetCANBusData() { return (CANBusData.getValue()) ? String("OK") : String("Failed");}
    boolData WifiInit;
    String GetWifiInit() { return (WifiInit.getValue()) ? String("OK") : String("Failed");}
    boolData WifiConnected;
    String GetWifiConnected() { 
        if (WiFi.getMode() == WIFI_MODE_AP) 
            return "AP Mode";
        else
            return (WifiConnected.getValue()) ? String("OK") : String("Down");
        }
    boolData MQTTConnected;
    String GetMQTTConnected() { return (MQTTConnected.getValue()) ? String("OK") : String("Down");}
    boolData WebServerState;
    String GetWebServerState() { return (WebServerState.getValue()) ? String("Started") : String("Stopped");}
    uint16Data ChargeVolts;
    String GetChargeVolts() { return (String) _roundFloat(ChargeVolts.getValue()*0.001) + "V" ;}
    uint32Data ChargeAmps;
    String GetChargeAmps() { return (String) _roundUInt32(ChargeAmps.getValue()*0.001) + "A";}
    uint16Data DischargeVolts;
    String GetDischargeVolts() { return (String) _roundFloat(DischargeVolts.getValue()*0.001) + "V";}
    uint32Data DischargeAmps;
    String GetDischargeAmps() { return (String) _roundUInt32(DischargeAmps.getValue()*0.001) + "A";}
    boolData ChargeEnable;
    String GetChargeEnable() { return (ChargeEnable.getValue()) ? String("Yes") : String("No");}
    boolData DischargeEnable;
    String GetDischargeEnable(){ return (DischargeEnable.getValue()) ? String("Yes") : String("No");}
    uint8Data BattSOC;
    String GetBattSOC() { return (String) BattSOC.getValue() + "%";}
    uint16Data BattVolts ;
    String GetBattVolts() { return (String) _roundFloat(BattVolts.getValue()*0.01) + "V";}
    int32Data BattAmps;
    String GetBattAmps() { return (String) _roundInt(BattAmps.getValue()*0.001) + "A";}
    boolData LittleFSMounted;
    String GetLittleFSMounted() { return (LittleFSMounted.getValue()) ? String("Online") : String("Offline"); }
    stringData IPAddr;
    String GetIPAddress() { return IPAddr.getValue(); }
    boolData ForceCharging;
    String GetForceCharging() { return (ForceCharging.getValue() == true) ? String("Yes") : String("No"); }
};


class Display{
private:
    int _screen = 0;
    int16_t _height = 0;
    int16_t _width = 0;
    int _textSize;
    int _textFont;
    uint8_t _textLocation;
    uint16_t _backColour;
    uint16_t _textColour;
    uint16_t _headerSize;
    uint16_t Line1;
    uint16_t Line2;
    uint16_t Line3;
    uint16_t Line4;
    uint16_t Line5;
    uint16_t Line6;
    uint16_t Line7;
    uint16_t Line8;

public:

    bool StartupTimeout = false;

    enum Screen {
        StartUp = 0,
        Values = 1,
        Normal = 2
    };

    DisplayData Data;
    void Begin();
    void Begin(int Size, int Font);
    void ClearScreen();
    void Write(String Text);
    void Write(String Text, int Size);
    void Write(String Text, int Size, uint16_t Colour);
    void WriteStringXY(String Text, int16_t X, int16_t Y);
    void WriteStringXY(String Text, int16_t X, int16_t Y, int TextSize);
    void WriteStringXY(String Text, int16_t X, int16_t Y, int TextSize, uint16_t Colour);
    void WriteStringValue(String Text, uint16_t Line, uint16_t Colour);
    void WriteStringValue(String Text, uint16_t Line, uint16_t Colour, int TextSize);
    
    void WriteLine(String Line);
    void WriteLine(String Line, int Size);
    void WriteLine(String Line, int Size, uint16_t Colour);    
    void SetPosition(int16_t X, int16_t Y);
    void SetTextSize(int Size);
    void SetTextFont(int Font);
    void SetTextSize(int Size, bool Store);
    void SetTextFont(int Font, bool Store);
    void SetTextColour(uint16_t Colour);
    void SetTextColour(uint16_t Colour, u_int16_t BackgroundColour);
    void SetBackgroundColour(uint16_t Colour);
    void SetTextLocation(uint8_t Location);
    int GetWidth();
    int GetHeight();
    void SetWidth(int16_t Width);
    void SetHeight(int16_t Height);
    void SetScreen(Screen Number);
    Screen GetScreen(){ return (Screen) _screen;} 
    void UpdateScreenValues();
    void RefreshScreen();
    void NextScreen();
    void PreviousScreen();

};
