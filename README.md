Latest source has a breaking change (Jan 2025), the wifi and MQTT details are now stored in a seperate area of the NVS, so if using an older version on upgrade the details will need to be put back in using the AP setup method.

Please also note, this is a hobby project and I add features as and when I have time, it means sometimes it's not updated for a while and then sometimes I add a number of features!

This is the new V2 version of https://github.com/sijones/VE.DirectMQTTCANBUS, this will be the version for the future and is actively being developed.

The new UI means no coding knowledge is required, just flash and set up.

DiyBatteryBMS takes data from a Victron Smart Shunt and sends it to a inverter over CAN using Pylontech protocol allowing for "DIY LifePO4" Batteries to be integrated.

The Victron Smart Shunt provides the actual monitoring data, this software translates it to Pylontech protocol that most Inverters understand, the part that you need to configure is the charging voltage, current that is also sent to the Inverter.

As long as the Inverter accepts Pylontech protocol over CAN Bus this software should work with it, it's tested with Solis Inverters but forum users have reported it working with others.

The data is also sent over MQTT and allows commands to be sent back to control Charge/Discharge/Force Charge.

This software supports:
- ESP32 developer boards with MCP2515 CAN Bus adapter
- ESP32 with built-in CAN controller
- **NEW**: ESP32-S3 with built-in CAN controller (using TWAI driver)

If you don't use the Lilygo CAN485 board, you will need to add a CAN transceiver to your ESP32 or ESP32-S3 board.

See the WIKI for more detailed documentation.

- See also: https://www.victronenergy.com/live/vedirect_protocol:faq

With the help of the MQTT server you can integrate the monitoring data to virtually any Home Automation System. I use Home Assistant to automate off peak battery charging (using Force Charge) and can also enable and disable the charging and discharging.

The device itself no longer requires internet as the NTP Servers are now configurable, the web browser accessing this device does need internet access as the framework for the web page is loaded from a Content Delivery Network.

## Features
- Setup from a browser, flash to your ESP32 device then go to http://192.168.4.1 and connect it to your wifi, once connected go to http://diy-batterybms.local or it's IP address to configure all settings.
- Listen to VE.Direct messages and publish some of the information to a MQTT broker<br> The MQTT Topic is fully configurable.
- **NEW**: Home Assistant MQTT Discovery - Automatically creates all sensors and switches in Home Assistant with no manual configuration required
- Supports MQTT Commands to enable and disable charge/discharging of an inverter, force charge the batteries to be able to charge over night at off peak rates. See the home assistant file for the commands and config (optional - discovery creates these automatically).
- Supports single MQTT server
- OTA (Over The Air Update)<br> use your browser and go to http://IPADDRESS/update and upload the lastest binary - please note the donation button does not donate to me.
- LCD Screen Support (LCD 20x4 via I2C)
- Configurable NTP Server - This is for a future feature.

### Home Assistant Integration
The device now supports **MQTT Discovery** which automatically creates all entities when connected:
- **Sensors**: Battery SOC (%), Voltage (V), Current (A), Charge/Discharge Current Limits, Free Heap
- **Binary Sensors**: Charge/Discharge/Force Charge status indicators  
- **Switches**: Charge Enable, Discharge Enable, Force Charge, PylonTech Protocol Enable
- All entities are grouped under one "DIY Battery BMS" device
- No manual YAML configuration required (HomeAssistant.yaml is now optional for reference only)
- Discovery messages published automatically on MQTT connect

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
esp32dev, esp32plus, lilygo CAN485, ESP32-S3 with built-in CAN (TWAI).

Please use the recommended hardware, as a personal project it's difficult to support other configurations.

**NEW**: ESP32-S3 support with built-in CAN controller (TWAI) is now available.

## Recommended PIN Configuration

### esp32dev Environment
- **CAN_BUS_CS_PIN**: 2
- **CAN0_INT**: 22
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32

### esp32plus Environment
- **CAN_BUS_CS_PIN**: 5
- **CAN0_INT**: 13
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32

### esp32-ESPCAN Environment (Built-in CAN)
- **CAN_TX_PIN**: 27
- **CAN_RX_PIN**: 26
- **CAN_EN_PIN**: 23
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32

### esp32s3-ESPCAN Environment (Built-in CAN)
- **CAN_TX_PIN**: 27
- **CAN_RX_PIN**: 26
- **CAN_EN_PIN**: 23
- **VEDIRECT_RX**: 33
- **VEDIRECT_TX**: 32

### esp32c3-ESPCAN Environment (Built-in CAN)
- **CAN_TX_PIN**: 6
- **CAN_RX_PIN**: 7
- **CAN_EN_PIN**: 5
- **VEDIRECT_RX**: 21
- **VEDIRECT_TX**: 20

**Note**: These PINs can be configured through the web interface after flashing. The forbidden GPIO lists for each environment prevent selection of pins that should not be used.

Links:

esp32dev
https://www.amazon.co.uk/gp/product/B0C9THDPXP/

esp32plus
https://www.amazon.co.uk/dp/B0BHZ8H6LM

Lilygo CAN485
https://www.aliexpress.com/item/1005003624034092.html

LCD
https://www.amazon.co.uk/dp/B07V5K3ZVB

Isolated CAN Bus Adapter
https://www.amazon.co.uk/Coolwell-Isolated-Expansion-Raspberry-SN65HVD230/dp/B0C7VX6G6P

Non-Isolated Adapter - Does work and cheaper but can be blown if not careful.
https://www.amazon.co.uk/AZDelivery-MCP2515-Receiver-Development-Compatible/dp/B086TXSFD8/

Victron Smart Shunt
https://www.amazon.co.uk/Victron-Energy-SmartShunt-Battery-Bluetooth/dp/B0856PHNLX

## Disclaimer
I WILL NOT BE HELD LIABLE FOR ANY DAMAGE THAT YOU DO TO YOU, ONE OF YOUR DEVICES, BURN YOUR HOUSE DOWN, ETC.
A CERTAIN LEVEL OF KNOWLEDGE IS EXPECTED, LIKE HOW TO WIRE THINGS AND PROGRAM THE SOFTWARE TO YOUR DEVICE.
