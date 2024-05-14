This is the new V2 version of https://github.com/sijones/VE.DirectMQTTCANBUS, this will be the version for the future and is actively being developed.

The new UI means no coding knowledge is required, just flash and set up.

DiyBatteryBMS takes data from a Victron Smart Shunt and sends it to a inverter over CAN allowing for "DIY LifePO4" Batteries to be integrated.

The data is also sent over MQTT and allows commands to be sent back to control Charge/Discharge/Force Charge.

This software uses a ESP32 developers board with a MCP2515 Can Bus adapter currently developed using Visual Studio code.

The software sends the data in Pylontech Protocol, most inverters should support this.

- See also: https://www.victronenergy.com/live/vedirect_protocol:faq

With the help of the MQTT server you can integrate the monitoring data to virtually any Home Automation System. I use Home Assistant to automate off peak battery charging (using Force Charge) and can also enable and disable the charging and discharging.

## Features
- Setup from a browser, flash to your ESP32 device then go to http://192.168.4.1 and connect it to your wifi, connected go to it's IP address to configure all settings.
- Listen to VE.Direct messages and publish some of the information to a MQTT broker<br> The MQTT Topic is fully configurable.
- Supports MQTT Commands to enable and disable charge/discharging of an inverter, force charge the batteries to be able to charge over night at off peak rates. See the home assistant file for the commands and config.
- Supports single MQTT server
- OTA (Over The Air Update)<br> use your browser and go to http://IPADDRESS/update and upload the lastest binary - please note the donation button does not donate to me.

## Features to come:
- Voltage Limited Charging, automatically reducing charge current to keep the voltage stable
- MQTT to CAN BUS support, use esphome BMS intgrations to feed the data in and send to the inverter.
- Multi Inverter support
- OneWire temperature sensors for charge control
- Temperature monitoring of batteries and inverter to run heaters and fans.
- LCD Screen Support

## Limitations
- VE.Direct2MQTT is only listening to messages of the VE.Direct device<br>It understands only the "ASCII" part of the protocol that is only good to receive a set of values. You can't request any special data or change any parameters of the VE.Direct device.<br>

## Hardware & Software Installation
See the Wiki page

## Hardware Recommended
A esp32dev or the esp32plus

Links:
** Disclosure, these are affliate links which will give me some money if you buy using them - this will help fund further developement **

esp32dev
https://amzn.to/3ypBQsv

esp32plus
https://amzn.to/3R43zWv

LCD
https://amzn.to/3WDjRJb

Victron Smart Shunt
https://amzn.to/3JV64q7

## Disclaimer
I WILL NOT BE HELD LIABLE FOR ANY DAMAGE THAT YOU DO TO YOU OR ONE OF YOUR DEVICES.
