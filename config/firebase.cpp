#include "firebase.h"
#include "firebase_secrets.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <ArduinoJson.h>
#include <new>

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);

// Firebase components
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;
static bool firebaseInitialized = false;
static bool firebaseReadyLogged = false;
static unsigned long lastFirebaseInitAttemptMs = 0;
static volatile unsigned long lastFirebaseWriteSuccessMs = 0;
static volatile unsigned long lastFirebaseReadyMs = 0;
static const unsigned long FIREBASE_INIT_RETRY_MS = 5000;
static const unsigned long FIREBASE_READY_WATCHDOG_MS = 10000;
static const unsigned long FIREBASE_FORCE_RETRY_MS = 10000;
static const unsigned long FIREBASE_WRITE_BACKOFF_START_MS = 500;
static const unsigned long FIREBASE_WRITE_BACKOFF_MAX_MS = 8000;
static const unsigned long FIREBASE_DELAY_SLICE_MS = 50;

enum FirebaseWriteType : uint8_t { FB_WRITE_CONTROLLER = 0, FB_WRITE_EVENT = 1 };

struct FirebaseWriteItem {
  uint32_t id = 0;
  FirebaseWriteType type = FB_WRITE_CONTROLLER;
  String deviceCode;
  String penCode;
  bool online = false;
  uint32_t lastSeenEpoch = 0;
  String source;
  String eventType;
  String payload;
  uint32_t eventEpoch = 0;
};

struct FirebaseWriteResult {
  uint32_t id = 0;
  FirebaseWriteType type = FB_WRITE_CONTROLLER;
  bool success = false;
};

static const size_t FIREBASE_QUEUE_LEN = 12;
static const size_t FIREBASE_RESULT_LEN = 8;

#if defined(ESP32)
static QueueHandle_t firebaseQueue = nullptr;
static TaskHandle_t firebaseTaskHandle = nullptr;
#else
static FirebaseWriteItem *firebaseQueueRing[FIREBASE_QUEUE_LEN];
static size_t firebaseQueueHead = 0;
static size_t firebaseQueueCount = 0;
#endif

static uint32_t firebaseNextRequestId = 1;

static FirebaseWriteResult firebaseResults[FIREBASE_RESULT_LEN];
static size_t firebaseResultHead = 0;
static size_t firebaseResultCount = 0;
#if defined(ESP32)
static portMUX_TYPE firebaseResultMux = portMUX_INITIALIZER_UNLOCKED;
#endif

static void firebaseRecordResult(uint32_t id, FirebaseWriteType type, bool success) {
#if defined(ESP32)
  portENTER_CRITICAL(&firebaseResultMux);
#endif
  firebaseResults[firebaseResultHead] = {id, type, success};
  firebaseResultHead = (firebaseResultHead + 1) % FIREBASE_RESULT_LEN;
  if (firebaseResultCount < FIREBASE_RESULT_LEN) {
    firebaseResultCount++;
  }
#if defined(ESP32)
  portEXIT_CRITICAL(&firebaseResultMux);
#endif
}

static bool firebaseFindResult(uint32_t requestId, FirebaseWriteType type, bool *successOut) {
  bool found = false;
#if defined(ESP32)
  portENTER_CRITICAL(&firebaseResultMux);
#endif
  for (size_t i = 0; i < firebaseResultCount; ++i) {
    const size_t idx = (firebaseResultHead + FIREBASE_RESULT_LEN - 1 - i) % FIREBASE_RESULT_LEN;
    if (firebaseResults[idx].id == requestId && firebaseResults[idx].type == type) {
      if (successOut) {
        *successOut = firebaseResults[idx].success;
      }
      found = true;
      break;
    }
  }
#if defined(ESP32)
  portEXIT_CRITICAL(&firebaseResultMux);
#endif
  return found;
}

static void firebaseMarkReady() {
  lastFirebaseReadyMs = millis();
  if (!firebaseReadyLogged) {
    firebaseReadyLogged = true;
    Serial.println("Firebase ready");
  }
}

static void firebasePumpApp() {
  if (!firebaseInitialized) {
    return;
  }

  app.loop();
  if (app.ready()) {
    firebaseMarkReady();
  }
}

static bool firebaseNoRecentSuccess(unsigned long now, unsigned long thresholdMs) {
  const unsigned long lastSuccessMs = lastFirebaseWriteSuccessMs;
  if (lastSuccessMs == 0) {
    return firebaseInitialized && now - lastFirebaseInitAttemptMs >= thresholdMs;
  }
  return now - lastSuccessMs >= thresholdMs;
}

static bool firebaseReadyStalled(unsigned long now) {
  if (!firebaseInitialized) {
    return false;
  }

  if (lastFirebaseReadyMs == 0) {
    return now - lastFirebaseInitAttemptMs >= FIREBASE_READY_WATCHDOG_MS;
  }

  return now - lastFirebaseReadyMs >= FIREBASE_READY_WATCHDOG_MS;
}

static void firebaseRetryDelay(unsigned long delayMs) {
  unsigned long remainingMs = delayMs;
  while (remainingMs > 0) {
    firebasePumpApp();
    const unsigned long sliceMs = remainingMs > FIREBASE_DELAY_SLICE_MS ? FIREBASE_DELAY_SLICE_MS : remainingMs;
#if defined(ESP32)
    vTaskDelay(pdMS_TO_TICKS(sliceMs));
#else
    delay(sliceMs);
    yield();
#endif
    remainingMs -= sliceMs;
  }
}

static void firebaseResetApp(const char *reason) {
  const unsigned long now = millis();
  if (!firebaseInitialized || now - lastFirebaseInitAttemptMs < FIREBASE_INIT_RETRY_MS) {
    return;
  }

  Serial.printf("Firebase watchdog reset: %s\n", reason);
  aClient.stopAsync(true);
  Database.resetApp();
  deinitializeApp(app);
  firebaseInitialized = false;
  firebaseReadyLogged = false;
  lastFirebaseReadyMs = 0;
  lastFirebaseInitAttemptMs = now;
}

static bool firebaseQueuePush(FirebaseWriteItem *item) {
#if defined(ESP32)
  if (!firebaseQueue) {
    firebaseQueue = xQueueCreate(FIREBASE_QUEUE_LEN, sizeof(FirebaseWriteItem *));
    if (!firebaseQueue) {
      Serial.println("Firebase queue creation failed");
      return false;
    }
  }

  if (xQueueSend(firebaseQueue, &item, 0) == pdTRUE) {
    return true;
  }

  FirebaseWriteItem *dropped = nullptr;
  if (xQueueReceive(firebaseQueue, &dropped, 0) == pdTRUE && dropped) {
    Serial.printf("Firebase queue full, overwriting oldest request %lu with %lu\n",
                  static_cast<unsigned long>(dropped->id),
                  static_cast<unsigned long>(item->id));
    firebaseRecordResult(dropped->id, dropped->type, false);
    delete dropped;
    if (xQueueSend(firebaseQueue, &item, 0) == pdTRUE) {
      return true;
    }
  }

  Serial.printf("Firebase queue push failed for request %lu\n", static_cast<unsigned long>(item->id));
  return false;
#else
  if (firebaseQueueCount >= FIREBASE_QUEUE_LEN) {
    FirebaseWriteItem *dropped = firebaseQueueRing[firebaseQueueHead];
    firebaseQueueHead = (firebaseQueueHead + 1) % FIREBASE_QUEUE_LEN;
    firebaseQueueCount--;
    if (dropped) {
      Serial.printf("Firebase queue full, overwriting oldest request %lu with %lu\n",
                    static_cast<unsigned long>(dropped->id),
                    static_cast<unsigned long>(item->id));
      firebaseRecordResult(dropped->id, dropped->type, false);
      delete dropped;
    }
  }
  const size_t idx = (firebaseQueueHead + firebaseQueueCount) % FIREBASE_QUEUE_LEN;
  firebaseQueueRing[idx] = item;
  firebaseQueueCount++;
  return true;
#endif
}

static FirebaseWriteItem *firebaseQueuePop(uint32_t timeoutMs) {
#if defined(ESP32)
  if (!firebaseQueue) {
    return nullptr;
  }
  FirebaseWriteItem *item = nullptr;
  if (xQueueReceive(firebaseQueue, &item, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
    return item;
  }
  return nullptr;
#else
  (void)timeoutMs;
  if (firebaseQueueCount == 0) {
    return nullptr;
  }
  FirebaseWriteItem *item = firebaseQueueRing[firebaseQueueHead];
  firebaseQueueHead = (firebaseQueueHead + 1) % FIREBASE_QUEUE_LEN;
  firebaseQueueCount--;
  return item;
#endif
}

static bool firebasePerformControllerWrite(const FirebaseWriteItem &item) {
  if (item.deviceCode.length() == 0 || item.penCode.length() == 0 || item.source.length() == 0) {
    Serial.println("Firebase controller write aborted: missing required fields");
    return false;
  }

  const String path = "/controllers/" + item.deviceCode;
  unsigned long backoffMs = FIREBASE_WRITE_BACKOFF_START_MS;

  while (true) {
    firebasePumpApp();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("Firebase controller write waiting for Wi-Fi: %s (retry in %lu ms)\n",
                    item.deviceCode.c_str(),
                    backoffMs);
      firebaseRetryDelay(backoffMs);
      backoffMs = min(backoffMs * 2, FIREBASE_WRITE_BACKOFF_MAX_MS);
      continue;
    }

    if (!firebaseReady()) {
      if (firebaseReadyStalled(millis())) {
        firebaseResetApp("app not ready during controller write");
      }
      Serial.printf("Firebase controller write waiting for readiness: %s (retry in %lu ms)\n",
                    item.deviceCode.c_str(),
                    backoffMs);
      firebaseRetryDelay(backoffMs);
      backoffMs = min(backoffMs * 2, FIREBASE_WRITE_BACKOFF_MAX_MS);
      continue;
    }

    StaticJsonDocument<192> doc;
    doc["device_code"] = item.deviceCode;
    doc["pen_code"] = item.penCode;
    doc["online"] = item.online;
    doc["last_seen_epoch"] = item.lastSeenEpoch;
    doc["last_seen_source"] = item.source;

    String json;
    serializeJson(doc, json);

    if (Database.set<object_t>(aClient, path, object_t(json))) {
      lastFirebaseWriteSuccessMs = millis();
      return true;
    }

    Serial.printf("Firebase controller write failed: %s (retry in %lu ms)\n",
                  path.c_str(),
                  backoffMs);
    if (firebaseNoRecentSuccess(millis(), FIREBASE_FORCE_RETRY_MS)) {
      firebaseResetApp("no successful write for >10 seconds");
      backoffMs = FIREBASE_WRITE_BACKOFF_START_MS;
    } else if (backoffMs < FIREBASE_WRITE_BACKOFF_MAX_MS) {
      backoffMs = min(backoffMs * 2, FIREBASE_WRITE_BACKOFF_MAX_MS);
    }

    firebaseRetryDelay(backoffMs);
  }
}

static bool firebasePerformLogEventWrite(const FirebaseWriteItem &item) {
  if (item.deviceCode.length() == 0 || item.eventType.length() == 0) {
    Serial.println("Firebase event write aborted: missing required fields");
    return false;
  }

  const String path = "/controller_events/" + item.deviceCode + "/" + String(item.eventEpoch);
  unsigned long backoffMs = FIREBASE_WRITE_BACKOFF_START_MS;

  while (true) {
    firebasePumpApp();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("Firebase event write waiting for Wi-Fi: %s (retry in %lu ms)\n",
                    path.c_str(),
                    backoffMs);
      firebaseRetryDelay(backoffMs);
      backoffMs = min(backoffMs * 2, FIREBASE_WRITE_BACKOFF_MAX_MS);
      continue;
    }

    if (!firebaseReady()) {
      if (firebaseReadyStalled(millis())) {
        firebaseResetApp("app not ready during event write");
      }
      Serial.printf("Firebase event write waiting for readiness: %s (retry in %lu ms)\n",
                    path.c_str(),
                    backoffMs);
      firebaseRetryDelay(backoffMs);
      backoffMs = min(backoffMs * 2, FIREBASE_WRITE_BACKOFF_MAX_MS);
      continue;
    }

    StaticJsonDocument<320> doc;
    doc["device_code"] = item.deviceCode;
    doc["pen_code"] = item.penCode;
    doc["event_type"] = item.eventType;
    doc["payload"] = item.payload;
    doc["event_epoch"] = item.eventEpoch;

    String json;
    serializeJson(doc, json);

    if (Database.set<object_t>(aClient, path, object_t(json))) {
      lastFirebaseWriteSuccessMs = millis();
      return true;
    }

    Serial.printf("Firebase event write failed: %s (retry in %lu ms)\n",
                  path.c_str(),
                  backoffMs);
    if (firebaseNoRecentSuccess(millis(), FIREBASE_FORCE_RETRY_MS)) {
      firebaseResetApp("no successful write for >10 seconds");
      backoffMs = FIREBASE_WRITE_BACKOFF_START_MS;
    } else if (backoffMs < FIREBASE_WRITE_BACKOFF_MAX_MS) {
      backoffMs = min(backoffMs * 2, FIREBASE_WRITE_BACKOFF_MAX_MS);
    }

    firebaseRetryDelay(backoffMs);
  }
}

#if defined(ESP32)
static void firebaseTaskMain(void *param) {
  (void)param;
  const TickType_t idleDelay = pdMS_TO_TICKS(20);
  while (true) {
    firebasePumpApp();

    if (WiFi.status() == WL_CONNECTED && firebaseReadyStalled(millis())) {
      firebaseResetApp("worker loop stalled");
    }

    if (!firebaseInitialized) {
      vTaskDelay(idleDelay);
      continue;
    }

    FirebaseWriteItem *item = firebaseQueuePop(0);
    if (!item) {
      vTaskDelay(idleDelay);
      continue;
    }

    bool ok = false;
    if (item->type == FB_WRITE_CONTROLLER) {
      ok = firebasePerformControllerWrite(*item);
    } else if (item->type == FB_WRITE_EVENT) {
      ok = firebasePerformLogEventWrite(*item);
    }

    firebaseRecordResult(item->id, item->type, ok);
    delete item;
  }
}

static void firebaseStartWorkerIfNeeded() {
  if (firebaseTaskHandle) {
    return;
  }
  if (!firebaseQueue) {
    firebaseQueue = xQueueCreate(FIREBASE_QUEUE_LEN, sizeof(FirebaseWriteItem *));
    if (!firebaseQueue) {
      return;
    }
  }
  xTaskCreatePinnedToCore(firebaseTaskMain,
                          "firebaseTask",
                          6144,
                          nullptr,
                          1,
                          &firebaseTaskHandle,
                          1);
}
#endif

void firebaseStartup() {
  if (firebaseInitialized) {
    return;
  }

  // Configure SSL client
  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  // Initialize Firebase
  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
  firebaseInitialized = true;
  firebaseReadyLogged = false;
  lastFirebaseReadyMs = 0;

#if defined(ESP32)
  firebaseStartWorkerIfNeeded();
#endif
}

void processData(AsyncResult &aResult) {
  if (!aResult.isResult())
    return;

  if (aResult.isEvent())
    Firebase.printf("Event task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.eventLog().message().c_str(), aResult.eventLog().code());

  if (aResult.isDebug())
    Firebase.printf("Debug task: %s, msg: %s\n", aResult.uid().c_str(), aResult.debug().c_str());

  if (aResult.isError())
    Firebase.printf("Error task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());

  if (aResult.available())
    Firebase.printf("task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());
}

void runFirebase() {
  if (!firebaseInitialized) {
    if (WiFi.status() != WL_CONNECTED) {
      return;
    }

    const unsigned long now = millis();
    if (now - lastFirebaseInitAttemptMs < FIREBASE_INIT_RETRY_MS) {
      return;
    }

    lastFirebaseInitAttemptMs = now;
    firebaseStartup();
    return;
  }

#if defined(ESP32)
  firebaseStartWorkerIfNeeded();
#else
  firebasePumpApp();
  if (WiFi.status() == WL_CONNECTED && firebaseReadyStalled(millis())) {
    firebaseResetApp("main loop stalled");
    return;
  }

  FirebaseWriteItem *item = firebaseQueuePop(0);
  if (item) {
    bool ok = false;
    if (item->type == FB_WRITE_CONTROLLER) {
      ok = firebasePerformControllerWrite(*item);
    } else if (item->type == FB_WRITE_EVENT) {
      ok = firebasePerformLogEventWrite(*item);
    }
    firebaseRecordResult(item->id, item->type, ok);
    delete item;
  }
#endif
}

bool firebaseReady() {
  return firebaseInitialized && app.ready();
}

bool firebaseQueueUpsertController(const String &deviceCode,
                                   const String &penCode,
                                   bool online,
                                   uint32_t lastSeenEpoch,
                                   const String &source,
                                   uint32_t *requestIdOut) {
  if (deviceCode.length() == 0 || penCode.length() == 0 || source.length() == 0) {
    return false;
  }

  FirebaseWriteItem *item = new (std::nothrow) FirebaseWriteItem();
  if (!item) {
    Serial.println("Firebase controller queue allocation failed");
    return false;
  }

  item->id = firebaseNextRequestId++;
  item->type = FB_WRITE_CONTROLLER;
  item->deviceCode = deviceCode;
  item->penCode = penCode;
  item->online = online;
  item->lastSeenEpoch = lastSeenEpoch;
  item->source = source;

  if (!firebaseQueuePush(item)) {
    Serial.printf("Firebase controller queue failed for %s\n", deviceCode.c_str());
    delete item;
    return false;
  }

  if (requestIdOut) {
    *requestIdOut = item->id;
  }
  return true;
}

bool firebaseQueueLogEvent(const String &deviceCode,
                           const String &penCode,
                           const String &eventType,
                           const String &payload,
                           uint32_t eventEpoch,
                           uint32_t *requestIdOut) {
  if (deviceCode.length() == 0 || eventType.length() == 0) {
    return false;
  }

  FirebaseWriteItem *item = new (std::nothrow) FirebaseWriteItem();
  if (!item) {
    Serial.println("Firebase event queue allocation failed");
    return false;
  }

  item->id = firebaseNextRequestId++;
  item->type = FB_WRITE_EVENT;
  item->deviceCode = deviceCode;
  item->penCode = penCode;
  item->eventType = eventType;
  item->payload = payload;
  item->eventEpoch = eventEpoch;

  if (!firebaseQueuePush(item)) {
    Serial.printf("Firebase event queue failed for %s\n", deviceCode.c_str());
    delete item;
    return false;
  }

  if (requestIdOut) {
    *requestIdOut = item->id;
  }
  return true;
}

bool firebaseCheckControllerWrite(uint32_t requestId, bool *successOut) {
  return firebaseFindResult(requestId, FB_WRITE_CONTROLLER, successOut);
}

bool firebaseCheckLogEventWrite(uint32_t requestId, bool *successOut) {
  return firebaseFindResult(requestId, FB_WRITE_EVENT, successOut);
}

bool firebaseUpsertController(const String &deviceCode,
                              const String &penCode,
                              bool online,
                              uint32_t lastSeenEpoch,
                              const String &source) {
  return firebaseQueueUpsertController(deviceCode, penCode, online, lastSeenEpoch, source, nullptr);
}

bool firebaseLogEvent(const String &deviceCode,
                      const String &penCode,
                      const String &eventType,
                      const String &payload,
                      uint32_t eventEpoch) {
  return firebaseQueueLogEvent(deviceCode, penCode, eventType, payload, eventEpoch, nullptr);
}

unsigned long firebaseLastSuccessfulUpdateMs() {
  return lastFirebaseWriteSuccessMs;
}

bool firebaseWriteStale(unsigned long thresholdMs) {
  return firebaseNoRecentSuccess(millis(), thresholdMs);
}
