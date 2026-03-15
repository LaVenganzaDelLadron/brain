#include "firebase.h"
#include "firebase_secrets.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
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
static const unsigned long FIREBASE_INIT_RETRY_MS = 5000;

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

  app.loop();

  if (app.ready() && !firebaseReadyLogged) {
    firebaseReadyLogged = true;
    Serial.println("Firebase ready");
  }
}

bool firebaseReady() {
  return firebaseInitialized && app.ready();
}

bool firebaseUpsertController(const String &deviceCode, const String &penCode, bool online, uint32_t lastSeenEpoch, const String &source) {
  if (!firebaseReady()) {
    return false;
  }

  if (deviceCode.length() == 0 || penCode.length() == 0 || source.length() == 0) {
    return false;
  }

  const String path = "/controllers/" + deviceCode;
  const String json = String("{\"device_code\":\"") + deviceCode +
                      "\",\"pen_code\":\"" + penCode +
                      "\",\"online\":" + (online ? "true" : "false") +
                      ",\"last_seen_epoch\":" + String(lastSeenEpoch) +
                      ",\"last_seen_source\":\"" + source + "\"}";

  const bool status = Database.set<object_t>(aClient, path, object_t(json));
  if (!status) {
    Serial.printf("Firebase upsert failed: %s\n", path.c_str());
  }
  return status;
}

bool firebaseLogEvent(const String &deviceCode, const String &penCode, const String &eventType, const String &payload, uint32_t eventEpoch) {
  if (!firebaseReady()) {
    return false;
  }

  const String path = "/controller_events/" + deviceCode + "/" + String(eventEpoch);
  const String json = String("{\"device_code\":\"") + deviceCode +
                      "\",\"pen_code\":\"" + penCode +
                      "\",\"event_type\":\"" + eventType +
                      "\",\"payload\":\"" + payload +
                      "\",\"event_epoch\":" + String(eventEpoch) + "}";
  return Database.set<object_t>(aClient, path, object_t(json));
}
