#include "VEDisplay.h"

//LCD_I2C _lcd(0x27, 20, 4);
LCD_I2C * _lcd;

void Display::Begin(DisplayType _DisplayType){

    enabled = true;
    if (_DisplayType == LCD2004){
        if (_lcd != NULL) delete _lcd;
        _lcd = new LCD_I2C(0x27,20,4);
        _height = 4;
        _width = 20;
    }
    _lcd->createChar(0, heart);
    _lcd->begin();
    _lcd->display();
    _lcd->backlight();
    ClearScreen();
}

void Display::ClearScreen(){
    if(!enabled) return;
    _lcd->clear();
}

uint8_t Display::GetHeight()
{ return _height; }

uint8_t Display::GetWidth()
{ return _width; }

void Display::SetHeight(uint8_t Height)
{ _height = Height; }

void Display::SetWidth(uint8_t Width)
{ _width = Width; }

void Display::SetPosition(uint8_t X, uint8_t Y){
    if(!enabled) return;
   _lcd->setCursor(X,Y);
}

void Display::Write(String Text){
    if(!enabled) return;
    _lcd->print(Text);
}

void Display::WriteLine(String Text){
    if(!enabled) return;
    _lcd->println(Text);
}

void Display::WriteLineXY(String Text, uint8_t X, uint8_t Y){
    if(!enabled) return;
    SetPosition(X,Y);
    _lcd->println(Text);
}

void Display::WriteStringXY(String Text, uint8_t X, uint8_t Y){
    if(!enabled) return;
     SetPosition(X,Y);
     Write(Text);
}

void Display::WriteSpecialXY(uint8_t CustomChar, uint8_t X, uint8_t Y)
{
    if(!enabled) return;
    SetPosition(X,Y);
    _lcd->write(0);
}

void Display::SetScreen(Screen Number){
   if(!enabled) return;
    ClearScreen();
    switch (Number)
    {

    case StartUp :
        _screen = StartUp;
        WriteStringXY("VE   :",0,0);
        WriteStringXY((Data.VEData._currentValue == true) ? "OK":"No",7,0);
        WriteStringXY("Wifi :",11,0);
        WriteStringXY((Data.WifiConnected._currentValue==true) ? "OK":"No",18,0);
        WriteStringXY("CAN I:", 0, 1);
        WriteStringXY((Data.CANInit._currentValue == true) ? "OK":"No",7,1);
        WriteStringXY("CAN D:",11,1);
        WriteStringXY((Data.CANBusData._currentValue==true) ? "OK":"No",18,1);
        WriteStringXY("MQTT :",0,2);
        WriteStringXY((Data.MQTTConnected._currentValue==true) ? "OK":"No",7,2);
        WriteStringXY("L FS :", 11, 2);
        WriteStringXY((Data.LittleFSMounted._currentValue==true) ? "OK":"No",18,2);
        WriteStringXY("IP: " + Data.IPAddr._currentValue, 0, 3);
       // WriteStringXY("Web Services:", 0, Line7);                
        break;
    case Values :
        _screen = Values;
        WriteStringXY("SOC:    ", 0, 1);
        WriteStringXY(Data.GetBattSOC(),9-Data.GetBattSOC().length(),1);
        WriteStringXY("BV:     ", 10, 1);
        WriteStringXY(Data.GetBattVolts(),20-Data.GetBattVolts().length(),1);
        WriteStringXY("BC:   ", 0, 2);
        WriteStringXY(Data.GetBattAmps(),10-Data.GetBattAmps().length(),2);
/*
        WriteStringXY("Discharge Amps:", 0, Line4);

        WriteStringXY("Charge Enabled:", 0, Line5);

        WriteStringXY("Discharge Enabled:", 0, Line6);

        WriteStringXY("Force Charging:", 0,Line7);
   */
        break;

    case Normal :
        _screen = Normal;
    /*    SetTextLocation(TC_DATUM);
        SetTextColour(_textColour);
        SetTextSize(3);
        WriteStringXY("Battery Status", (_width / 2), 0, _textSize+1);
        SetTextSize(_textSize);
        SetTextLocation(TL_DATUM);
        WriteStringXY("SOC:", 0, Line2);        
        WriteStringXY("Batt Volts:", 0, Line4);
        WriteStringXY("Batt Amps:", 0, Line6); */
        break;
    default:
        break;
    }
    
}

void Display::UpdateScreenValues(){
    if(!enabled) return;
    // X = Hoz / Y Vertical
    
    //   WriteStringXY("IP Add: " + Data.IPAddr._currentValue, 0, 4);

/*
    WriteStringXY("VE   :",0,0);
    WriteStringXY("Wifi: ",11,0);
    WriteStringXY("CAN I: ", 0, 1);
    WriteStringXY("CAN D: ",11,1);
    WriteStringXY("MQTT: ",0,2);
    WriteStringXY("L FS: ", 11, 2);
  */  

    switch (GetScreen()){
    case StartUp: // Start Up
        if(Data.VEData.hasChanged()){
            WriteStringXY((Data.VEData._currentValue == true) ? "OK":"No",7,0);
        }
        if(Data.CANInit.hasChanged()){
            WriteStringXY((Data.CANInit._currentValue == true) ? "OK":"No",7,1);
        }
        if(Data.CANBusData.hasChanged()){
            WriteStringXY((Data.CANBusData._currentValue==true) ? "OK":"No",18,1);
        }
        if(Data.WifiConnected.hasChanged()){
            WriteStringXY((Data.WifiConnected._currentValue==true) ? "OK":"No",18,0);
        }
        if(Data.MQTTConnected.hasChanged()){
            WriteStringXY((Data.MQTTConnected._currentValue==true) ? "OK":"No",7,2);
        }
        if(Data.LittleFSMounted.hasChanged()){
            WriteStringXY((Data.LittleFSMounted._currentValue==true)?"OK":"No",18,2);
        }
        //if(Data.WebServerState.hasChanged()){
        //    M5.Display.fillRect(_width-_textClear,Line7,_width,Line7+M5.Display.fontHeight(),BLACK);
        //}
        if(Data.IPAddr.hasChanged()){
            WriteStringXY("IP: " + Data.IPAddr._currentValue, 0, 3);
        }
        break;
    case Values: // Normal
        WriteStringXY("DIY BATTERY BMS",0,0);
        switch (_runHeatbeat)
        {
        case 0:
            WriteSpecialXY(0,19,0);
            _runHeatbeat++;
            break;
        case 1:
            WriteStringXY(" ",19,0);
            _runHeatbeat=0;
            break;
        default:
            _runHeatbeat=0;
            break;
        }

        if(Data.BattSOC.hasChanged()){
            WriteStringXY("SOC:    ", 0, 1);
            WriteStringXY(Data.GetBattSOC(),10-Data.GetBattSOC().length(),1);
        }

        if(Data.BattVolts.hasChanged()){
            WriteStringXY("BV:     ", 10, 1);
            WriteStringXY(Data.GetBattVolts(),20-Data.GetBattVolts().length(),1);
        }
        
        if(Data.BattAmps.hasChanged()){
            WriteStringXY("BC:   ", 0, 2);
            WriteStringXY(Data.GetBattAmps(),10-Data.GetBattAmps().length(),2);
        }
        break;
    case Normal: // Config
        break;
  }


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