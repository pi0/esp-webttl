# Esp8266 WebTTL

A very simple &amp; fast WebTTL based on ESP8266 Wifi Module, coding by PlatformIO/Arduino.

The program running on an ESP8266 board allows you to access your device TTL remotely from any device that has a browser.

The project is fully functioning and the speed of controlling TTL is the same as you connect the cable via an USB-TTL device.

## Build steps

1. Install [PlatformIO](https://platformio.org/install/ide?install=vscode)
3. Build the project and upload to an ESP8266 board
4. Modify SSID information by copy `data/config.example.json` to `data/config.json`.
5. Build SPIFFS image with "Build Filesystem Image" and upload to the board.

