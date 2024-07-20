This is the new V2 version of https://github.com/sijones/VE.DirectMQTTCANBUS, this will be the version for the future and is actively being developed.

The new UI means no coding knowledge is required, just flash and set up.

DiyBatteryBMS takes data from a Victron Smart Shunt and sends it to a inverter over CAN using Pylontech protocol allowing for "DIY LifePO4" Batteries to be integrated.

The Victron Smart Shunt provides the actual monitoring data, this software translates it to Pylontech protocol that most Inverters understand, the part that you need to configure is the charging voltage, current that is also sent to the Inverter.

As long as the Inverter accepts Pylontech protocol over CAN Bus this software should work with it, it's tested with Solis Inverters but forum users have reported it working with others.

The data is also sent over MQTT and allows commands to be sent back to control Charge/Discharge/Force Charge.

This software uses a ESP32 developers board with a MCP2515 Can Bus adapter currently developed using Visual Studio code.

See the WIKI for more detailed documentation.

The software sends the data in Pylontech Protocol, most inverters should support this.

- See also: https://www.victronenergy.com/live/vedirect_protocol:faq

With the help of the MQTT server you can integrate the monitoring data to virtually any Home Automation System. I use Home Assistant to automate off peak battery charging (using Force Charge) and can also enable and disable the charging and discharging.

## Features
- Setup from a browser, flash to your ESP32 device then go to http://192.168.4.1 and connect it to your wifi, once connected go to http://diy-smartbms.local/ or it's IP address to configure all settings.
- Listen to VE.Direct messages and publish some of the information to a MQTT broker<br> The MQTT Topic is fully configurable.
- Supports MQTT Commands to enable and disable charge/discharging of an inverter, force charge the batteries to be able to charge over night at off peak rates. See the home assistant file for the commands and config.
- Supports single MQTT server
- OTA (Over The Air Update)<br> use your browser and go to http://IPADDRESS/update and upload the lastest binary - please note the donation button does not donate to me.
- LCD Screen Support (LCD 20x4 via I2C)

## Features to come:
- Voltage Limited Charging, automatically reducing charge current to keep the voltage stable
- MQTT to CAN BUS support, use esphome BMS intgrations to feed the data in and send to the inverter.
- Multi Inverter support
- OneWire temperature sensors for charge control
- Temperature monitoring of batteries and inverter to run heaters and fans.

## Limitations
- DiyBatteryBMS is only listening to messages of the VE.Direct device<br>It understands only the "ASCII" part of the protocol that is only good to receive a set of values. You can't request any special data or change any parameters of the VE.Direct device.<br>

## Hardware & Software Installation
See the Wiki page

## Hardware Recommended
esp32dev or the esp32plus

Links:
** Disclosure, these are affliate links which will give me some money if you buy using them - this will help fund further development **

esp32dev
https://amzn.to/3ypBQsv

esp32plus
https://amzn.to/3R43zWv

LCD
https://amzn.to/3WDjRJb

Isolated CAN Bus Adapter
https://amzn.to/45fyzbE

Non-Isolated Adapter - Does work and cheaper but can be blown if not careful.
https://amzn.to/3VxEy8F

Victron Smart Shunt
https://amzn.to/3JV64q7

## Disclaimer
I WILL NOT BE HELD LIABLE FOR ANY DAMAGE THAT YOU DO TO YOU, ONE OF YOUR DEVICES, BURN YOUR HOUSE DOWN, ETC.
A CERTAIN LEVEL OF KNOWLEDGE IS EXPECTED, LIKE HOW TO WIRE THINGS AND PROGRAM THE SOFTWARE TO YOUR DEVICE.
