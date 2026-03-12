#include "wifi.h"
#include "../config/firebase.h"
#include "rtc.h"
#include "../functions/device.h"
#include <ArduinoJson.h>

const char *ssid = "Virus";
const char *password = "BreakingCode!=5417";

const char *ap_ssid = "SmartHogWifi";
const char *ap_password = "12345678";

static const uint16_t CONTROLLER_PORT = 3333;
WiFiServer controllerServer(CONTROLLER_PORT);
WiFiClient controllerClient;
String controllerRxLine;
String lastDeviceCode;
String lastPenCode;
String lastSeenSource;
uint32_t lastSeenEpoch = 0;
unsigned long lastHeartbeatRxMs = 0;
unsigned long lastOfflineWriteRetryMs = 0;
unsigned long lastOnlineWriteRetryMs = 0;
bool controllerMarkedOnline = false;
bool pendingOnlineWrite = false;
unsigned long lastStaRetryMs = 0;
String lastPostedDeviceCode;
String lastPostedPenCode;
String pendingPostDeviceCode;
String pendingPostPenCode;
bool pendingDevicePost = false;
unsigned long lastDevicePostAttemptMs = 0;
unsigned long devicePostBackoffMs = 0;

static const unsigned long CONTROLLER_OFFLINE_TIMEOUT_MS = 12000;
static const unsigned long OFFLINE_RETRY_MS = 1000;
static const unsigned long ONLINE_RETRY_MS = 1000;
static const unsigned long STA_RETRY_MS = 3000;
static const unsigned long DEVICE_POST_BACKOFF_START_MS = 2000;
static const unsigned long DEVICE_POST_BACKOFF_MAX_MS = 30000;

uint32_t currentEpoch() {
  const uint32_t rtcEpoch = rtcUnixTime();
  if (rtcEpoch > 0) {
    return rtcEpoch;
  }
  return 1700000000UL + static_cast<uint32_t>(millis() / 1000UL);
}

void scheduleDevicePost(const String &deviceCode, const String &penCode) {
  if (deviceCode.length() == 0 || penCode.length() == 0) {
    return;
  }

  if (!pendingDevicePost &&
      deviceCode == lastPostedDeviceCode &&
      penCode == lastPostedPenCode) {
    return;
  }

  if (pendingDevicePost &&
      deviceCode == pendingPostDeviceCode &&
      penCode == pendingPostPenCode) {
    return;
  }

  pendingDevicePost = true;
  pendingPostDeviceCode = deviceCode;
  pendingPostPenCode = penCode;
  devicePostBackoffMs = DEVICE_POST_BACKOFF_START_MS;
  lastDevicePostAttemptMs = 0;
  Serial.printf("Queued device post for %s / %s\n", deviceCode.c_str(), penCode.c_str());
}

void processPendingDevicePost(unsigned long now) {
  if (!pendingDevicePost) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (lastDevicePostAttemptMs != 0 &&
      now - lastDevicePostAttemptMs < devicePostBackoffMs) {
    return;
  }

  lastDevicePostAttemptMs = now;
  Serial.printf("Posting device to API: %s / %s\n",
                pendingPostDeviceCode.c_str(),
                pendingPostPenCode.c_str());

  const bool ok = postDeviceToApi(pendingPostDeviceCode, pendingPostPenCode);
  if (ok) {
    lastPostedDeviceCode = pendingPostDeviceCode;
    lastPostedPenCode = pendingPostPenCode;
    pendingDevicePost = false;
    devicePostBackoffMs = DEVICE_POST_BACKOFF_START_MS;
    Serial.printf("Device post succeeded for %s\n", lastPostedDeviceCode.c_str());
  } else {
    if (devicePostBackoffMs < DEVICE_POST_BACKOFF_MAX_MS) {
      devicePostBackoffMs = min(devicePostBackoffMs * 2, DEVICE_POST_BACKOFF_MAX_MS);
    }
    Serial.printf("Device post failed; retry in %lu ms\n", devicePostBackoffMs);
  }
}

bool parseControllerHeartbeat(const String &payload,
                              String &deviceCode,
                              String &penCode,
                              bool &online,
                              uint32_t &lastSeenEpoch,
                              String &source) {
  StaticJsonDocument<320> doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Heartbeat JSON parse error: %s\n", err.c_str());
    return false;
  }

  const char *type = doc["type"] | "";
  if (strcmp(type, "controller_heartbeat") != 0) {
    return false;
  }

  deviceCode = String(doc["device_code"] | "");
  penCode = String(doc["pen_code"] | "");
  online = doc["online"] | false;
  lastSeenEpoch = doc["last_seen_epoch"] | 0;
  source = String(doc["last_seen_source"] | "");

  if (deviceCode.length() == 0 || penCode.length() == 0 || source.length() == 0) {
    Serial.println("Heartbeat missing required fields");
    return false;
  }

  return true;
}

void runControllerHub() {
  const unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastStaRetryMs >= STA_RETRY_MS) {
      lastStaRetryMs = now;
      Serial.println("Router uplink disconnected, retrying STA...");
      WiFi.reconnect();
    }
  }

  if (!controllerClient || !controllerClient.connected()) {
    if (controllerClient) {
      controllerClient.stop();
      controllerRxLine = "";
    }

    WiFiClient incoming = controllerServer.available();
    if (incoming) {
      controllerClient = incoming;
      controllerRxLine = "";
      controllerClient.setNoDelay(true);
      Serial.printf("Controller connected: %s\n", controllerClient.remoteIP().toString().c_str());
    }
  }
  if (controllerClient && controllerClient.connected()) {
    while (controllerClient.available()) {
      const char c = static_cast<char>(controllerClient.read());
      if (c == '\r') {
        continue;
      }

      if (c != '\n') {
        controllerRxLine += c;
        if (controllerRxLine.length() > 700) {
          Serial.println("Heartbeat line too long; dropping");
          controllerRxLine = "";
        }
        continue;
      }

      if (controllerRxLine.length() == 0) {
        continue;
      }

      String deviceCode;
      String penCode;
      bool online = false;
      uint32_t heartbeatEpoch = 0;
      String source;

      const String payload = controllerRxLine;
      controllerRxLine = "";

      if (!parseControllerHeartbeat(payload, deviceCode, penCode, online, heartbeatEpoch, source)) {
        Serial.print("Ignored controller payload: ");
        Serial.println(payload);
        continue;
      }

      lastDeviceCode = deviceCode;
      lastPenCode = penCode;
      lastSeenSource = source;
      lastSeenEpoch = heartbeatEpoch;
      lastHeartbeatRxMs = now;
      controllerMarkedOnline = true;
      pendingOnlineWrite = true;
      scheduleDevicePost(deviceCode, penCode);

      const bool ok = firebaseUpsertController(deviceCode, penCode, true, heartbeatEpoch, source);
      if (ok) {
        pendingOnlineWrite = false;
      }
      Serial.printf("Controller heartbeat %s for %s\n", ok ? "stored" : "failed", deviceCode.c_str());
    }
  }

  if (controllerMarkedOnline && lastDeviceCode.length() > 0) {
    if (now - lastHeartbeatRxMs > CONTROLLER_OFFLINE_TIMEOUT_MS) {
      if (controllerClient && controllerClient.connected()) {
        Serial.println("Controller heartbeat timeout, dropping stale socket");
        controllerClient.stop();
        controllerRxLine = "";
      }

      if (now - lastOfflineWriteRetryMs >= OFFLINE_RETRY_MS) {
        lastOfflineWriteRetryMs = now;
        const uint32_t offlineEpoch = currentEpoch();
        const bool ok = firebaseUpsertController(lastDeviceCode, lastPenCode, false, offlineEpoch, "brain_timeout");
        if (ok) {
          controllerMarkedOnline = false;
          pendingOnlineWrite = false;
          lastSeenEpoch = offlineEpoch;
          lastSeenSource = "brain_timeout";
          Serial.printf("Controller marked offline: %s\n", lastDeviceCode.c_str());
        }
      }
    } else if (pendingOnlineWrite && lastSeenEpoch > 0 && lastSeenSource.length() > 0) {
      if (now - lastOnlineWriteRetryMs >= ONLINE_RETRY_MS) {
        lastOnlineWriteRetryMs = now;
        const bool ok = firebaseUpsertController(lastDeviceCode, lastPenCode, true, lastSeenEpoch, lastSeenSource);
        if (ok) {
          pendingOnlineWrite = false;
          Serial.printf("Controller online state synced: %s\n", lastDeviceCode.c_str());
        }
      }
    }
  }

  processPendingDevicePost(now);
}

void startup() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  controllerServer.begin();

  Serial.println("Hotspot started");
  Serial.print("Hotspot IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.printf("Controller hub listening on TCP %u\n", CONTROLLER_PORT);

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
}
