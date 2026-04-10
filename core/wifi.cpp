#include "wifi.h"
#include "../config/firebase.h"
#include "rtc.h"
#include "../functions/device.h"
#include <ArduinoJson.h>

const char *ssid = "caleb_5G";
const char *password = "caleb121617";

const char *ap_ssid = "SmartHogWifi";
const char *ap_password = "12345678";

static const uint16_t CONTROLLER_PORT = 3333;
WiFiServer controllerServer(CONTROLLER_PORT);
WiFiClient controllerClient;
bool controllerWasConnected = false;
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
uint32_t pendingOnlineWriteId = 0;
uint32_t pendingOfflineWriteId = 0;
uint32_t pendingDevicePostId = 0;
uint32_t pendingOnlineWriteEpoch = 0;
uint32_t pendingOfflineWriteEpoch = 0;
String pendingOnlineWriteSource;
String pendingOfflineWriteSource;
String pendingOnlineWriteDevice;
String pendingOnlineWritePen;
String pendingOfflineWriteDevice;
String pendingOfflineWritePen;
String registeredDeviceCode;
String registeredPenCode;
String pendingPostDeviceCode;
String pendingPostPenCode;
bool pendingDevicePost = false;
bool deviceRegistered = false;
unsigned long lastDevicePostAttemptMs = 0;
unsigned long devicePostBackoffMs = 0;
unsigned long lastStaAttemptMs = 0;
unsigned long staBackoffMs = 0;
bool staWasConnected = false;
wl_status_t lastStaStatus = WL_IDLE_STATUS;

static const unsigned long CONTROLLER_OFFLINE_TIMEOUT_MS = 12000;
static const unsigned long OFFLINE_RETRY_MS = 1000;
static const unsigned long ONLINE_RETRY_MS = 1000;
static const unsigned long STA_BACKOFF_START_MS = 3000;
static const unsigned long STA_BACKOFF_MAX_MS = 30000;
static const unsigned long DEVICE_POST_BACKOFF_START_MS = 2000;
static const unsigned long DEVICE_POST_BACKOFF_MAX_MS = 30000;
static const unsigned long FIREBASE_STALE_RETRY_MS = 10000;

uint32_t currentEpoch() {
  const uint32_t rtcEpoch = rtcUnixTime();
  if (rtcEpoch > 0) {
    return rtcEpoch;
  }
  return 1700000000UL + static_cast<uint32_t>(millis() / 1000UL);
}

static bool isRegisteredDevice(const String &deviceCode, const String &penCode) {
  return deviceRegistered &&
         deviceCode == registeredDeviceCode &&
         penCode == registeredPenCode;
}

void scheduleDevicePost(const String &deviceCode, const String &penCode) {
  if (deviceCode.length() == 0 || penCode.length() == 0) {
    return;
  }

  if (isRegisteredDevice(deviceCode, penCode)) {
    return;
  }

  if (pendingDevicePost &&
      deviceCode == pendingPostDeviceCode &&
      penCode == pendingPostPenCode) {
    return;
  }

  if (pendingDevicePostId != 0) {
    return;
  }

  pendingDevicePost = true;
  pendingPostDeviceCode = deviceCode;
  pendingPostPenCode = penCode;
  devicePostBackoffMs = DEVICE_POST_BACKOFF_START_MS;
  lastDevicePostAttemptMs = 0;
  Serial.printf("Queued device post for %s / %s\n", deviceCode.c_str(), penCode.c_str());
}

void processFirebaseWriteResults() {
  if (pendingOnlineWriteId != 0) {
    bool success = false;
    if (firebaseCheckControllerWrite(pendingOnlineWriteId, &success)) {
      if (success) {
        if (pendingOnlineWriteDevice == lastDeviceCode &&
            pendingOnlineWritePen == lastPenCode &&
            pendingOnlineWriteEpoch == lastSeenEpoch &&
            pendingOnlineWriteSource == lastSeenSource) {
          pendingOnlineWrite = false;
        }
        Serial.printf("Controller online update confirmed: %s\n", pendingOnlineWriteDevice.c_str());
      } else {
        Serial.printf("Controller online update failed: %s\n", pendingOnlineWriteDevice.c_str());
      }
      pendingOnlineWriteId = 0;
    }
  }

  if (pendingOfflineWriteId != 0) {
    bool success = false;
    if (firebaseCheckControllerWrite(pendingOfflineWriteId, &success)) {
      if (success) {
        controllerMarkedOnline = false;
        pendingOnlineWrite = false;
        lastSeenEpoch = pendingOfflineWriteEpoch;
        lastSeenSource = pendingOfflineWriteSource;
        Serial.printf("Controller marked offline: %s\n", pendingOfflineWriteDevice.c_str());
      } else {
        Serial.printf("Controller offline update failed: %s\n", pendingOfflineWriteDevice.c_str());
      }
      pendingOfflineWriteId = 0;
    }
  }
}

void processPendingDevicePost(unsigned long now) {
  if (pendingDevicePostId != 0) {
    bool success = false;
    if (devicePostResult(pendingDevicePostId, &success)) {
      pendingDevicePostId = 0;
      if (success) {
        registeredDeviceCode = pendingPostDeviceCode;
        registeredPenCode = pendingPostPenCode;
        deviceRegistered = true;
        pendingDevicePost = false;
        devicePostBackoffMs = DEVICE_POST_BACKOFF_START_MS;
        Serial.printf("Device registered for %s\n", registeredDeviceCode.c_str());
      } else {
        if (devicePostBackoffMs < DEVICE_POST_BACKOFF_MAX_MS) {
          devicePostBackoffMs = min(devicePostBackoffMs * 2, DEVICE_POST_BACKOFF_MAX_MS);
        }
        Serial.printf("Device post failed; retry in %lu ms\n", devicePostBackoffMs);
      }
    } else {
      return;
    }
  }

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

  if (pendingDevicePostId != 0) {
    return;
  }

  lastDevicePostAttemptMs = now;
  Serial.printf("Queueing device post: %s / %s\n",
                pendingPostDeviceCode.c_str(),
                pendingPostPenCode.c_str());

  uint32_t requestId = 0;
  const bool queued = queueDevicePost(pendingPostDeviceCode, pendingPostPenCode, &requestId);
  if (queued) {
    pendingDevicePostId = requestId;
  } else {
    if (devicePostBackoffMs < DEVICE_POST_BACKOFF_MAX_MS) {
      devicePostBackoffMs = min(devicePostBackoffMs * 2, DEVICE_POST_BACKOFF_MAX_MS);
    }
    Serial.printf("Device post queue failed; retry in %lu ms\n", devicePostBackoffMs);
  }
}

void maintainWiFiConnection() {
  const unsigned long now = millis();
  const wl_status_t status = WiFi.status();

  if (status != lastStaStatus) {
    Serial.printf("WiFi status: %d\n", status);
    lastStaStatus = status;
  }

  if (status == WL_CONNECTED) {
    if (!staWasConnected) {
      staWasConnected = true;
      staBackoffMs = STA_BACKOFF_START_MS;
      Serial.print("WiFi connected, IP: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  if (staWasConnected) {
    staWasConnected = false;
    Serial.println("WiFi disconnected; attempting reconnect");
  }

  if (now - lastStaAttemptMs < staBackoffMs) {
    return;
  }

  lastStaAttemptMs = now;
  Serial.printf("WiFi reconnecting (backoff %lu ms)\n", staBackoffMs);
  WiFi.disconnect();
  yield();
  WiFi.begin(ssid, password);

  if (staBackoffMs < STA_BACKOFF_MAX_MS) {
    staBackoffMs = min(staBackoffMs * 2, STA_BACKOFF_MAX_MS);
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
  processFirebaseWriteResults();

  if (controllerMarkedOnline &&
      lastDeviceCode.length() > 0 &&
      lastSeenEpoch > 0 &&
      firebaseWriteStale(FIREBASE_STALE_RETRY_MS)) {
    pendingOnlineWrite = true;
  }

  if (!controllerClient || !controllerClient.connected()) {
    if (controllerClient && controllerWasConnected) {
      controllerWasConnected = false;
      Serial.println("Controller disconnected");
    }
    if (controllerClient) {
      controllerClient.stop();
      controllerRxLine = "";
    }

    WiFiClient incoming = controllerServer.available();
    if (incoming) {
      controllerClient = incoming;
      controllerWasConnected = true;
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

      if (pendingOnlineWriteId == 0) {
        uint32_t requestId = 0;
        const bool queued = firebaseQueueUpsertController(deviceCode, penCode, true, heartbeatEpoch, source, &requestId);
        if (queued) {
          pendingOnlineWriteId = requestId;
          pendingOnlineWriteEpoch = heartbeatEpoch;
          pendingOnlineWriteSource = source;
          pendingOnlineWriteDevice = deviceCode;
          pendingOnlineWritePen = penCode;
        }
        Serial.printf("Controller heartbeat %s for %s\n", queued ? "queued" : "queue_failed", deviceCode.c_str());
      }
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
        if (pendingOfflineWriteId == 0) {
          lastOfflineWriteRetryMs = now;
          const uint32_t offlineEpoch = currentEpoch();
          uint32_t requestId = 0;
          const bool queued = firebaseQueueUpsertController(lastDeviceCode,
                                                            lastPenCode,
                                                            false,
                                                            offlineEpoch,
                                                            "brain_timeout",
                                                            &requestId);
          if (queued) {
            pendingOfflineWriteId = requestId;
            pendingOfflineWriteEpoch = offlineEpoch;
            pendingOfflineWriteSource = "brain_timeout";
            pendingOfflineWriteDevice = lastDeviceCode;
            pendingOfflineWritePen = lastPenCode;
          } else {
            Serial.printf("Controller offline queue failed for %s\n", lastDeviceCode.c_str());
          }
        }
      }
    } else if (pendingOnlineWrite && lastSeenEpoch > 0 && lastSeenSource.length() > 0) {
      if (now - lastOnlineWriteRetryMs >= ONLINE_RETRY_MS) {
        if (pendingOnlineWriteId == 0) {
          lastOnlineWriteRetryMs = now;
          uint32_t requestId = 0;
          const bool queued = firebaseQueueUpsertController(lastDeviceCode,
                                                            lastPenCode,
                                                            true,
                                                            lastSeenEpoch,
                                                            lastSeenSource,
                                                            &requestId);
          if (queued) {
            pendingOnlineWriteId = requestId;
            pendingOnlineWriteEpoch = lastSeenEpoch;
            pendingOnlineWriteSource = lastSeenSource;
            pendingOnlineWriteDevice = lastDeviceCode;
            pendingOnlineWritePen = lastPenCode;
          } else {
            Serial.printf("Controller online queue failed for %s\n", lastDeviceCode.c_str());
          }
        }
      }
    }
  }

  processPendingDevicePost(now);
}

void startup() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  staBackoffMs = STA_BACKOFF_START_MS;
  staWasConnected = false;
  lastStaAttemptMs = 0;
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
