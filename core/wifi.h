#ifndef WIFI_H
#define WIFI_H

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

void startup();
void runControllerHub();
void maintainWiFiConnection();

#endif
