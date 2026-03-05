#include "firebase.h"
#include "firebase_secrets.h"

UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);

// Firebase components
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;

void firebaseStartup() {
// Configure SSL client
  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);
  
  // Initialize Firebase
  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
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
  app.loop();
}

bool firebaseReady() {
  return app.ready();
}

bool firebaseUpsertController(const String &deviceCode, const String &penCode, bool online, uint32_t lastSeenEpoch, const String &source) {
  (void)deviceCode;
  (void)penCode;
  (void)online;
  (void)lastSeenEpoch;
  (void)source;
  return false;
}

bool firebaseLogEvent(const String &deviceCode, const String &penCode, const String &eventType, const String &payload, uint32_t eventEpoch) {
  (void)deviceCode;
  (void)penCode;
  (void)eventType;
  (void)payload;
  (void)eventEpoch;
  return false;
}
