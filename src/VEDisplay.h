#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LCD-I2C.h>

#define icon_heart 0
#define icon_chg_en 1
#define icon_dischg_en 2
#define icon_fc 3
#define icon_chg 4
#define icon_dischg 5
#define icon_float 6
#define icon_blank " "
#define Line1 0
#define Line2 1
#define Line3 2
#define Line4 3


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
    String _currentValue ="";
    String _oldValue="";
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
    void setValue(int32_t Value) { if(_currentValue != Value) {_oldValue = _currentValue; _currentValue = Value; _changed = true;} }
    bool hasChanged() { if (_changed) {_changed = false; return true; } else return false; }
};

// Class to store the real time display data
// it converts it on call for the right format to display
class DisplayData
{
public:
    float _roundUInt32(uint32_t f) { return round(f * 10.0) / 10.0; }
    float _roundFloat(float f) { return round(f * 10.0) / 10.0; }
    float _roundInt(int32_t f) { return roundf(f * 10.0) / 10.0; }

    boolData VEData;
    String GetVEData(){ return (VEData.getValue()) ? String("OK") : String("No"); }
    boolData CANInit;
    String GetCANInit() { return (CANInit.getValue()) ? String("OK") : String("No");}
    boolData CANBusData;
    String GetCANBusData() { return (CANBusData.getValue()) ? String("OK") : String("No");}
    boolData WifiInit;
    String GetWifiInit() { return (WifiInit.getValue()) ? String("OK") : String("No");}
    boolData WifiConnected;
    String GetWifiConnected() { 
        if (WiFi.getMode() == WIFI_MODE_AP) 
            return "AP";
        else
            return (WifiConnected.getValue()) ? String("OK") : String("No");
        }
    boolData MQTTConnected;
    String GetMQTTConnected() { return (MQTTConnected.getValue()) ? String("OK") : String("NO");}
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
    String GetChargeEnable() { return (ChargeEnable.getValue()) ? String("Yes") : String(" No");}
    boolData DischargeEnable;
    String GetDischargeEnable(){ return (DischargeEnable.getValue()) ? String("Yes") : String(" No");}
    uint8Data BattSOC;
    String GetBattSOC() { return (String) BattSOC.getValue() + "%";}
    uint16Data BattVolts ;
    String GetBattVolts() { return (String) _roundFloat(BattVolts.getValue()*0.01) + "V";}
    int32Data BattAmps;
    String GetBattAmps() { 
        String _battAmps = String(BattAmps.getValue()*0.1);
        _battAmps.remove(_battAmps.length()-1);
        return _battAmps + "A";}
    boolData LittleFSMounted;
    String GetLittleFSMounted() { return (LittleFSMounted.getValue()) ? String("OK") : String("No"); }
    stringData IPAddr;
    String GetIPAddress() { return IPAddr.getValue(); }
    boolData ForceCharging;
    String GetForceCharging() { return (ForceCharging.getValue() == true) ? String("Yes") : String(" No"); }
};

class Display{
private:

    bool enabled = false;
    int _screen = 0;
    uint8_t _height = 4;
    uint8_t _width = 20;
    uint16_t _headerSize;
    u8_t _runHeatbeat = 0;
    uint8_t _heart[8] =  {0b00000,0b01010,0b11111,0b11111,0b01110,0b00100,0b00000,0b00000};
    uint8_t _chg_en[8] = {0b00100,0b01110,0b11111,0b00100,0b00100,0b00100,0b00100,0b00100};
    uint8_t _dischg_en[8] = {0b00100,0b00100,0b00100,0b00100,0b00100,0b11111,0b01110,0b00100};
    uint8_t _fc[8] =     {0b11100,0b10000,0b11000,0b10000,0b10111,0b00100,0b00100,0b00111};
    uint8_t _charge[8] = {0b00000,0b00000,0b00100,0b01010,0b10001,0b00100,0b01010,0b10001};
    uint8_t _dischg[8] = {0b10001,0b01010,0b00100,0b10001,0b01010,0b00100,0b00000,0b00000};
    uint8_t _float[8] =  {0b00000,0b00000,0b10101,0b01010,0b00000,0b01010,0b10101,0b00000};

public:

    bool StartupTimeout = false;

    enum Screen {
        StartUp = 0,
        Values = 1,
        Normal = 2
    };
    enum DisplayType {
        LCD2004 = 0
    };

    struct VEDisplay
    {
        DisplayType LCDType;
        uint16_t X;
        uint16_t Y;
    };
    
    //Display::Display(DisplayType);
    DisplayData Data;
    void Begin(DisplayType);
    bool Enabled(){return enabled;}
    void Enable(){Begin(LCD2004);SetScreen(StartUp);}
    void Disable(){ClearScreen(); enabled = false;}
    void ClearScreen();
    void Write(String Text);
    void WriteStringXY(String Text, uint8_t X, uint8_t Y);
    void WriteLine(String Line);
    void WriteLineXY(String Line,uint8_t X, uint8_t Y);
    void WriteSpecialXY(uint8_t CustomChar, uint8_t X, uint8_t Y);
    void SetPosition(uint8_t X, uint8_t Y);
    u8_t GetWidth();
    uint8_t GetHeight();
    void SetWidth(uint8_t Width);
    void SetHeight(uint8_t Height);
    void SetScreen(Screen Number);
    Screen GetScreen(){ return (Screen) _screen;} 
    void UpdateScreenValues();
    void RefreshScreen();
    void NextScreen();
    void PreviousScreen();

};
