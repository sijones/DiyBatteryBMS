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
    _lcd->begin();
    _lcd->display();
    _lcd->createChar(icon_heart, _heart);
    delay(5);
    _lcd->createChar(icon_happy, _happy);
    delay(5);
    _lcd->createChar(icon_sad, _sad);
    delay(5);
    _lcd->createChar(icon_fc, _fc);
    delay(5);
    _lcd->createChar(icon_chg,_charge);
    delay(5);
    _lcd->createChar(icon_dischg,_dischg);
    delay(5);
    _lcd->createChar(icon_float,_float);
    delay(5);
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
        WriteStringXY((Data.VEData._currentValue == true) ? "OK":"No",7,Line1);
        WriteStringXY("Wifi :",11,0);
        WriteStringXY((Data.WifiConnected._currentValue==true) ? "OK":"No",18,Line1);
        WriteStringXY("CAN I:", 0, 1);
        WriteStringXY((Data.CANInit._currentValue == true) ? "OK":"No",7,Line2);
        WriteStringXY("CAN D:",11,1);
        WriteStringXY((Data.CANBusData._currentValue==true) ? "OK":"No",18,Line2);
        WriteStringXY("MQTT :",0,2);
        WriteStringXY((Data.MQTTConnected._currentValue==true) ? "OK":"No",7,Line3);
        WriteStringXY("L FS :", 11, 2);
        WriteStringXY((Data.LittleFSMounted._currentValue==true) ? "OK":"No",18,Line3);
        WriteStringXY("IP: ", 0, Line4);
        WriteStringXY(Data.IPAddr._currentValue,19-Data.IPAddr._currentValue.length(),Line4);              
        break;
    case Values :
        _screen = Values;
        WriteStringXY("SOC:   ", 0, 0);
        WriteStringXY(Data.GetBattSOC(),9-Data.GetBattSOC().length(),0);
        WriteStringXY("BV:    ", 0, 1);
        WriteStringXY(Data.GetBattVolts(),9-Data.GetBattVolts().length(),1);
        WriteStringXY("BC:   ", 11, 1);
        WriteStringXY(Data.GetBattAmps(),20-Data.GetBattAmps().length(),1);
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

    // this is used to track how many Icons are being displayed
    uint8_t numIcons = 1;
    int32_t battAmps = Data.BattAmps._currentValue;
    
    switch (GetScreen()){
    case StartUp: // Start Up
        
        if(Data.VEData.hasChanged()){
            WriteStringXY((Data.VEData._currentValue == true) ? "OK":"No",7,Line1);
        }
        if(Data.CANInit.hasChanged()){
            WriteStringXY((Data.CANInit._currentValue == true) ? "OK":"No",7,Line2);
        }
        if(Data.CANBusData.hasChanged()){
            WriteStringXY((Data.CANBusData._currentValue==true) ? "OK":"No",18,Line2);
        }
        if(Data.WifiConnected.hasChanged()){
            WriteStringXY((Data.WifiConnected._currentValue==true) ? "OK":"No",18,Line1);
        }
        if(Data.MQTTConnected.hasChanged()){
            WriteStringXY((Data.MQTTConnected._currentValue==true) ? "OK":"No",7,Line3);
        }
        if(Data.LittleFSMounted.hasChanged()){
            WriteStringXY((Data.LittleFSMounted._currentValue==true)?"OK":"No",18,Line3);
        }
        //if(Data.WebServerState.hasChanged()){
        //    M5.Display.fillRect(_width-_textClear,Line7,_width,Line7+M5.Display.fontHeight(),BLACK);
        //}
        if(Data.IPAddr.hasChanged()){
            WriteStringXY("IP: ", 0, Line4);
            WriteStringXY(Data.IPAddr._currentValue,19-Data.IPAddr._currentValue.length(),Line4);
        }
        break;
    case Values: // Normal
        // Top Right Line Status Icons
        // Wipe the status bar to put new up.
        WriteStringXY("    ",15,Line1);
        switch (_runHeatbeat)
        {
        case 0:
            WriteSpecialXY(icon_heart,19,Line1);
            _runHeatbeat++;
            break;
        case 1:
            WriteStringXY(icon_blank,19,Line1);
            _runHeatbeat=0;
            break;
        default:
            _runHeatbeat=0;
            break;
        }

        // Display icon for Forced Charging if Active
        if(Data.ForceCharging.hasChanged()){
            if(Data.ForceCharging.getValue()==true){
                WriteSpecialXY(icon_fc,19-numIcons,Line1);
                numIcons++;
                }
        }
        // Display icon for battery state (charging/floating/discharging)
        if(battAmps>1000)
            {WriteSpecialXY(icon_chg,19-numIcons,Line1);
            numIcons++;}
        else if(battAmps<1000)
            {WriteSpecialXY(icon_dischg,19-numIcons,Line1);
            numIcons++;}
        else
            {WriteSpecialXY(icon_float,19-numIcons,Line1);
            numIcons++;}

        // End of Status Icon

        if(Data.BattSOC.hasChanged()){
            WriteStringXY("SOC:    ", 0, Line1);
            WriteStringXY(Data.GetBattSOC(),9-Data.GetBattSOC().length(),Line1);
        }

        if(Data.BattVolts.hasChanged()){
            WriteStringXY("BV:    ", 0, Line2);
            WriteStringXY(Data.GetBattVolts(),9-Data.GetBattVolts().length(),Line2);
        }
        
        if(Data.BattAmps.hasChanged()){
            WriteStringXY("BC:   ", 10, Line2);
            WriteStringXY(Data.GetBattAmps(),20-Data.GetBattAmps().length(),Line2);
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