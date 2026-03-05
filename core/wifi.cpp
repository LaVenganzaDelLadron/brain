#include "wifi.h"

const char *ssid = "Virus";
const char *password = "BreakingCode!=5417";

const char *ap_ssid = "SmartHogWifi";
const char *ap_password = "12345678";

static const uint16_t CONTROLLER_PORT = 3333;

void runControllerHub() {
  // Controller hub runtime is intentionally in this file for ESP32 brain build.
}

void startup() {
  WiFi.mode(WIFI_AP_STA);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 30000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to router!");
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Router connection timed out; AP-only mode remains active");
  }

  WiFi.softAP(ap_ssid, ap_password);

  Serial.println("Hotspot started");
  Serial.print("Hotspot IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.printf("Controller hub listening on TCP %u\n", CONTROLLER_PORT);
}
