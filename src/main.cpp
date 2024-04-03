
/*
    Esp WebTTL
    Copyright (c) 2022 Nate Zhang (skyvense@gmail.com)

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <EasyLed.h>
#include <TimeLib.h>
#include <CircularBuffer.h>
#include <SPI.h>

CircularBuffer<uint8_t, 2000> cached_screen_bytes; // A typical telnet screen is 80*25=2000

File record_file;
int record_file_opened = 0; // try once only at setup()
File root;

#define STATUS_LED 2
EasyLed led(STATUS_LED, EasyLed::ActiveLevel::Low, EasyLed::State::On); // Use this for an active-low LED

#define MAX_SRV_CLIENTS 5
WiFiServer telnet_server(23);
WiFiClient serverClients[MAX_SRV_CLIENTS];
AsyncWebServer web(80);
AsyncWebSocket ws("/ws");

int last_srv_clients_count = 0;
int flashing_ip = 0;
time_t last_active_time = 0;
int has_active = 0;

void BaseConfig();

struct EMPTY_SERIAL
{
  void println(const char *) {}
  void println(String) {}
  void printf(const char *, ...) {}
  void print(const char *) {}
  // void print(Printable) {}
  void begin(int) {}
  void end() {}
} _EMPTY_SERIAL;

// #define Serial_debug Serial
#define Serial_debug _EMPTY_SERIAL

#define Serial_peer Serial

struct Config
{
  String SSID = "S1";
  String Passwd = "abc";
  String Server = "192.168.8.1";
  String Token = "0000";
};
Config _config;

bool LoadConfig()
{
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile)
  {
    Serial_debug.println("Failed to open config file");
    return false;
  }
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  if (error)
  {
    Serial_debug.println("Failed to read file, using default configuration");
    return false;
  }
  _config.SSID = doc["SSID"] | "fail";
  _config.Passwd = doc["Passwd"] | "fail";
  if (_config.SSID == "fail" || _config.Passwd == "fail")
  {
    return false;
  }
  else
  {
    Serial_debug.println("Load wifi config from SPIFFS successful.");
    Serial_debug.print("Loaded ssid: ");
    Serial_debug.println(_config.SSID);
    Serial_debug.print("Loaded passwd: ");
    Serial_debug.println(_config.Passwd);
    return true;
  }
}

bool SaveConfig()
{
  DynamicJsonDocument doc(1024);
  JsonObject root = doc.to<JsonObject>();
  root["SSID"] = _config.SSID;
  root["Passwd"] = _config.Passwd;
  root["Server"] = _config.Server;
  root["Token"] = _config.Token;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile)
  {
    Serial_debug.println("Failed to open config file for writing");
    return false;
  }
  if (serializeJson(doc, configFile) == 0)
  {
    Serial_debug.println("Failed to write to file");
    return false;
  }
  return true;
}

void SmartConfig()
{
  Serial_debug.println("Use smart config to connect wifi.");
  WiFi.mode(WIFI_STA);
  WiFi.beginSmartConfig();
  while (1)
  {
    Serial_debug.println("Wait to connect wifi...");
    led.flash(10, 50, 50, 0, 0);
    delay(1000);
    if (WiFi.smartConfigDone())
    {
      Serial_debug.println("WiFi connected by smart config.");
      Serial_debug.print("SSID:");
      Serial_debug.println(WiFi.SSID());
      Serial_debug.print("IP Address:");
      Serial_debug.println(WiFi.localIP().toString());

      _config.SSID = WiFi.SSID();
      _config.Passwd = WiFi.psk();
      if (!SaveConfig())
      {
        Serial_debug.println("Failed to save config");
      }
      else
      {
        Serial_debug.println("Config saved");
      }
      break;
    }
  }
}

void ConnectWifi()
{
  if (LoadConfig())
  {
    BaseConfig();
  }
  else
  {
    SmartConfig();
  }
}

bool WiFiWatchDog()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    BaseConfig();
  }
  return true;
}

void BaseConfig()
{
  Serial_debug.println("Use base config to connect wifi.");
  led.flash(4, 125, 125, 0, 0);
  WiFi.mode(WIFI_STA);
  WiFi.begin(_config.SSID, _config.Passwd);
  int timeout = 30000;
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial_debug.println("Wait to connect wifi...");
    delay(500);
    timeout = timeout - 300;
    if (timeout <= 0)
    {
      Serial_debug.println("Wifi connect timeout, use smart config to connect...");
      SmartConfig();
      return;
    }

    led.flash(2, 125, 125, 0, 0);
  }
  Serial_debug.println("WiFi connected by base config.");
  Serial_debug.print("SSID:");
  Serial_debug.println(WiFi.SSID());
  Serial_debug.print("IP Address:");
  Serial_debug.println(WiFi.localIP().toString());
}

void onEvent(AsyncWebSocket *server1, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
  {
    has_active = 1;
    last_active_time = now();
    uint8_t buf[cached_screen_bytes.size()];
    CircularBuffer<uint8_t, 2000>::index_t i;
    for (i = 0; i < cached_screen_bytes.size(); i++)
    {
      buf[i] = cached_screen_bytes[i];
    }
    client->binary(buf, cached_screen_bytes.size());
    Serial_debug.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
  }
  break;
  case WS_EVT_DISCONNECT:
    Serial_debug.printf("WebSocket client #%u disconnected\n", client->id());
    break;
  case WS_EVT_DATA:
  {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
      Serial.write(data, len);
  }
  break;
  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  }
}

void initWebSocket()
{
  ws.onEvent(onEvent);
  web.addHandler(&ws);
}

void initFS()
{
  Serial_debug.println("Mounting FS...");
  if (!SPIFFS.begin())
  {
    Serial_debug.println("Failed to mount file system");
    return;
  }
}

void setup()
{
  led.off();

  Serial.setRxBufferSize(1024);
  Serial.swap();
  Serial.begin(115200);

  randomSeed(analogRead(0));
  initFS();
  ConnectWifi();
  initWebSocket();

  web.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
         { request->send(SPIFFS, "/index.html", "text/html"); });

  web.on("/b", HTTP_GET, [](AsyncWebServerRequest *request)
         {
    String inputMessage;
    int rate = 115200;
    if (request->hasParam("v"))
    {
      inputMessage = request->getParam("v")->value();
      rate = inputMessage.toInt();
      Serial.begin(rate);
      Serial.swap();
    }
    request->send(200, "text/plain", "OK"); });

  // web.serveStatic("/", SPIFFS, "/");
  web.begin();

  Serial_debug.println("Server started");

  led.on();
  last_active_time = now();
}

void CheckSerialData()
{
  if (Serial.available())
  {
    size_t len = Serial.available();
    uint8_t sbuf[len];
    Serial.readBytes(sbuf, len);
    if (len > 0)
    {
      ws.binaryAll(sbuf, len);
      led.flash(2, 20, 20, 0, 0);
      last_active_time = now();
      has_active = 1;
    }
  }
}

void loop(void)
{
  WiFiWatchDog();
  CheckSerialData();
}
