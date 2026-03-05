#include "wifi.h"

const char* ssid = "Virus";
const char* password = "BreakingCode!=5417";

const char* ap_ssid = "SmartHogWifi";
const char* ap_password = "12345678";


void startup() {
    WiFi.mode(WIFI_AP_STA);

    WiFi.begin(ssid, password);

    Serial.print("Connecting to WiFI");

    while(WiFi.status() != WL_CONNECTED) {
        delay(5000);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("Connected to router!");
    Serial.print("ESP32 IP: ");
    Serial.print(WiFi.localIP());

    WiFi.softAP(ap_ssid, ap_password);

    Serial.println("Hotspot Started");
    Serial.print("Hosspot IP: ");
    Serial.println(WiFi.softAPIP());
}




