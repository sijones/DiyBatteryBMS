
//#define M5STACK

#include "Arduino.h"
#include "VEDisplay.h"
#ifdef M5STACK
#include <M5Unified.h>
#include <M5UnitLCD.h>
#endif

void Display::Begin(int Size, int Font){
    #ifdef M5STACK
    _height = M5.Display.height();
    _width = M5.Display.width();
    _backColour = BLACK;
    _textColour = WHITE;
    M5.Display.setEpdMode(epd_mode_t::epd_text);
    SetTextFont(Font,true);
    // Here we calculate the lines, first with the Header
    SetTextSize(Size+1);
    _headerSize = M5.Display.fontHeight()+3;
    SetTextSize(Size,true);
    // Now calculate each line position
    Line1 = _headerSize + 2;
    uint16_t _fontHeight = M5.Display.fontHeight();
    Line2 = (Line1 + _fontHeight +2);
    Line3 = (Line2 + _fontHeight +2);
    Line4 = (Line3 + _fontHeight +2);
    Line5 = (Line4 + _fontHeight +2);
    Line6 = (Line5 + _fontHeight +2);
    Line7 = (Line6 + _fontHeight +2);
    Line8 = (Line7 + _fontHeight +2);
    #endif
    ClearScreen();

}

void Display::ClearScreen(){
    #ifdef M5STACK
    M5.Display.fillScreen(0);
    #endif
}

int Display::GetHeight()
{ return _height; }

int Display::GetWidth()
{ return _width; }

void Display::SetBackgroundColour(uint16_t Colour){
#ifdef M5STACK
    M5.Display.fillScreen(Colour);
#endif
}

void Display::SetTextLocation(uint8_t Location){
    _textLocation = Location;
    #ifdef M5STACK
    M5.Display.setTextDatum(_textLocation);
    #endif
}

void Display::SetHeight(int16_t Height)
{ _height = Height; }

void Display::SetWidth(int16_t Width)
{ _width = Width; }

void Display::SetTextSize(int Size)
{ SetTextSize(Size,false); }

void Display::SetTextSize(int Size, bool Store)
{ 
    if (Store) _textSize = Size; 
    #ifdef M5STACK
    M5.Display.setTextSize(_textSize); 
    #endif
}

void Display::SetTextFont(int Font)
{ SetTextFont(Font,false); }

void Display::SetTextFont(int Font, bool Store)
{ 
    if(Store) _textFont = Font; 
    #ifdef M5STACK
    M5.Display.setTextFont(Font); 
    #endif
}

void Display::SetPosition(int16_t X, int16_t Y){
    #ifdef M5STACK
    M5.Display.setCursor(X, Y);
    #endif 
}

void Display::SetTextColour(uint16_t Colour){
    #ifdef M5STACK
    M5.Display.setTextColor(Colour);
    #endif
}

void Display::SetTextColour(uint16_t Colour, uint16_t BackgroundColour){
    #ifdef M5STACK
    M5.Display.setTextColor(Colour, BackgroundColour);
    #endif
}

void Display::Write(String Text){
    #ifdef M5STACK
    Write(Text,_textSize);
    #endif
}

void Display::Write(String Text, int TextSize){
    #ifdef M5STACK
    SetTextSize(TextSize); 
    M5.Display.drawString(Text,M5.Display.getCursorX(),M5.Display.getCursorY());
    SetTextSize(_textSize); 
    #endif
}

void Display::WriteLine(String Text) {
    WriteLine(Text, _textSize);
}
void Display::WriteLine(String Text, int TextSize){
    #ifdef M5STACK
    SetTextSize(TextSize); 
    M5.Display.drawString(Text,M5.Display.getCursorX(),M5.Display.getCursorY());
    SetTextSize(_textSize);  
    #endif
}


void Display::WriteStringXY(String Text, int16_t X, int16_t Y){
    #ifdef M5STACK
    SetTextSize(_textSize);
    M5.Display.drawString(Text, (int32_t)X, (int32_t)Y);
    #endif    
}
void Display::WriteStringXY(String Text, int16_t X, int16_t Y, int TextSize){
    #ifdef M5STACK
    SetTextSize(TextSize);
    M5.Display.drawString(Text, (int32_t)X, (int32_t)Y);
    SetTextSize(_textSize);
    #endif
}

void Display::SetScreen(Screen Number){
   
    ClearScreen();
    #ifdef M5STACK
    switch (Number)
    {

    case StartUp :
        _screen = StartUp;
        SetTextLocation(TC_DATUM);
        SetTextColour(_textColour);
      //  SetTextSize(3);
        WriteStringXY("Services", (_width / 2), 0, _textSize+1);
      //  SetTextSize(_textSize);
        SetTextLocation(TL_DATUM);
        #ifdef M5STACK
        M5.Display.drawFastHLine(0,_headerSize, _width, RED);
        #endif
        WriteStringXY("Victron Data:", 0, Line1);
        WriteStringXY("CAN Bus Int:", 0, Line2);
        WriteStringXY("CAN Bus Data:", 0, Line3);
        WriteStringXY("Wifi Connected:", 0, Line4);
        WriteStringXY("MQTT Connected:", 0, Line5);
        WriteStringXY("LittleFS:", 0, Line6);
        WriteStringXY("Web Services:", 0, Line7);
        WriteStringXY("IP Address:", 0, Line8);
                
        break;
    case Values :
        _screen = Values;
        SetTextLocation(TC_DATUM);
        SetTextColour(_textColour, _backColour);
     //   SetTextSize(3);
        WriteStringXY("BMS Data", (_width / 2), 0, _textSize+1);
     //   SetTextSize(_textSize);
        #ifdef M5STACK
        M5.Display.drawFastHLine(0,_headerSize, _width, YELLOW);
        #endif
        SetTextLocation(TL_DATUM);
        WriteStringXY("Charge Volt:", 0, Line1);
        WriteStringXY("Charge Amps:", 0, Line2);
        WriteStringXY("Discharge Volts:", 0, Line3);
        WriteStringXY("Discharge Amps:", 0, Line4);
        WriteStringXY("Charge Enabled:", 0, Line5);
        WriteStringXY("Discharge Enabled:", 0, Line6);
        WriteStringXY("Force Charging:", 0,Line7);
        break;

    case Normal :
        _screen = Normal;
        SetTextLocation(TC_DATUM);
        SetTextColour(_textColour);
       // SetTextSize(3);
        WriteStringXY("Battery Status", (_width / 2), 0, _textSize+1);
       // SetTextSize(_textSize);
        #ifdef M5STACK
        M5.Display.drawFastHLine(0,_headerSize, _width, WHITE);
        #endif
        SetTextLocation(TL_DATUM);
        WriteStringXY("SOC:", 0, Line2);        
        WriteStringXY("Batt Volts:", 0, Line4);
        WriteStringXY("Batt Amps:", 0, Line6);
        break;
    default:
        break;
    }
    #endif
}

void Display::UpdateScreenValues(){
    // X = Hoz / Y Vertical
    #ifdef M5STACK
    int _textClear = M5.Display.textWidth("Stopped");
    
    switch (GetScreen()){
    case StartUp: // Start Up
        if(Data.VEData.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line1,_width,Line1+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetVEData(),Line1, WHITE);
        if(Data.CANInit.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line2,_width,Line2+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetCANInit(),Line2, WHITE);
        if(Data.CANBusData.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line3,_width,Line3+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetCANBusData(),Line3, WHITE);
        if(Data.WifiConnected.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line4,_width,Line4+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetWifiConnected(),Line4, WHITE);
        if(Data.MQTTConnected.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line5,_width,Line5+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetMQTTConnected(),Line5, WHITE);
        if(Data.LittleFSMounted.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line6,_width,Line6+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetLittleFSMounted(),Line6, WHITE);
        if(Data.WebServerState.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line7,_width,Line7+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetWebServerState(),Line7, WHITE);

        WriteStringValue(Data.GetIPAddress(),Line8, WHITE);
        
      break;
    case Values: // Normal
        if(Data.ChargeVolts.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line1,_width,Line1+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetChargeVolts(),Line1, WHITE);
        
        if(Data.ChargeAmps.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line2,_width,Line2+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetChargeAmps(),Line2, WHITE);

        if(Data.DischargeVolts.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line3,_width,Line3+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetDischargeVolts(),Line3, WHITE);

        if(Data.DischargeAmps.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line4,_width,Line4+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetDischargeAmps(),Line4, WHITE);

        if(Data.ChargeEnable.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line5,_width,Line5+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetChargeEnable(),Line5, WHITE);

        if(Data.DischargeEnable.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line6,_width,Line6+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetDischargeEnable(),Line6, WHITE);

        if(Data.ForceCharging.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line7,_width,Line7+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetForceCharging(),Line7, WHITE);


      break;
    case Normal: // Config
        if(Data.BattSOC.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line2,_width,Line2+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetBattSOC(),Line2, WHITE);
        if(Data.BattVolts.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line4,_width,Line4+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetBattVolts(),Line4, WHITE);
        if(Data.BattAmps.hasChanged()){
            M5.Display.fillRect(_width-_textClear,Line6,_width,Line6+M5.Display.fontHeight(),BLACK);
        }
        WriteStringValue(Data.GetBattAmps(),Line6, WHITE);
      break;
  }
#endif

}

void Display::WriteStringValue(String Text, uint16_t Line, uint16_t Colour, int TextSize)
{
    //SetTextLocation(TR_DATUM);
    //SetTextColour(Colour);
#ifdef M5STACK
    M5.Display.setTextSize(TextSize);
    M5.Display.setTextColor(Colour);
    int _textSizeofString = M5.Display.textWidth(Text);
    M5.Display.drawString(Text, (_width - _textSizeofString), Line);
    M5.Display.setTextColor(_textColour);
    M5.Display.setTextSize(_textSize);
#endif
    //WriteStringXY(Text,GetWidth(),Line);
    //SetTextLocation(_textLocation);
    //SetTextColour(_textColour);
}
void Display::WriteStringValue(String Text, uint16_t Line, uint16_t Colour)
{
    WriteStringValue(Text,Line,Colour, _textSize); 
}

void Display::NextScreen()
{
    if (_screen == Normal) 
        _screen = StartUp;
    else
        _screen++;
    SetScreen((Screen) _screen);
}

void Display::PreviousScreen()
{
    if (_screen == StartUp)
        _screen = Normal;
    else 
        _screen--;
    SetScreen((Screen) _screen);
}

void Display::RefreshScreen()
{
    SetScreen((Screen) _screen);
    UpdateScreenValues();
}