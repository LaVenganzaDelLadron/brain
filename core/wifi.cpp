#include "wifi.h"

// Optional local override. Define WIFI_STA_SSID / WIFI_STA_PASSWORD /
// WIFI_AP_SSID / WIFI_AP_PASSWORD in config/wifi_secrets.h.
#if __has_include("../config/wifi_secrets.h")
#include "../config/wifi_secrets.h"
#endif

#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID "caleb_5G"
#endif

#ifndef WIFI_STA_PASSWORD
#define WIFI_STA_PASSWORD "caleb121617"
#endif

#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "SmartHogWifi"
#endif

#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD "12345678"
#endif

static const unsigned long STA_BACKOFF_START_MS = 3000;
static const unsigned long STA_BACKOFF_MAX_MS = 30000;

static unsigned long lastStaAttemptMs = 0;
static unsigned long staBackoffMs = 0;
static bool staWasConnected = false;
static wl_status_t lastStaStatus = WL_IDLE_STATUS;

void controllerHubStartup();

namespace {

void logWiFiStatus(wl_status_t status) {
  if (status != lastStaStatus) {
    Serial.printf("WiFi status: %d\n", status);
    lastStaStatus = status;
  }
}

void markStationConnected() {
  if (staWasConnected) {
    return;
  }

  staWasConnected = true;
  staBackoffMs = STA_BACKOFF_START_MS;
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

void markStationDisconnected() {
  if (!staWasConnected) {
    return;
  }

  staWasConnected = false;
  Serial.println("WiFi disconnected; attempting reconnect");
}

void beginStationReconnect(unsigned long now) {
  lastStaAttemptMs = now;
  Serial.printf("WiFi reconnecting (backoff %lu ms)\n", staBackoffMs);
  WiFi.disconnect();
  yield();
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD);

  if (staBackoffMs < STA_BACKOFF_MAX_MS) {
    staBackoffMs = min(staBackoffMs * 2, STA_BACKOFF_MAX_MS);
  }
}

void startAccessPoint() {
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);

  Serial.println("Hotspot started");
  Serial.print("Hotspot IP: ");
  Serial.println(WiFi.softAPIP());
}

void connectStationOnStartup() {
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD);
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
    return;
  }

  Serial.println("Router connection timed out; AP-only mode remains active");
}

}  // namespace

void maintainWiFiConnection() {
  const unsigned long now = millis();
  const wl_status_t status = WiFi.status();

  logWiFiStatus(status);

  if (status == WL_CONNECTED) {
    markStationConnected();
    return;
  }

  markStationDisconnected();
  if (now - lastStaAttemptMs < staBackoffMs) {
    return;
  }

  beginStationReconnect(now);
}

void startup() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  staBackoffMs = STA_BACKOFF_START_MS;
  staWasConnected = false;
  lastStaAttemptMs = 0;
  lastStaStatus = WL_IDLE_STATUS;

  startAccessPoint();
  controllerHubStartup();
  connectStationOnStartup();
}
