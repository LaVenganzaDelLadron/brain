#ifndef FIREBASE_H
#define FIREBASE_H

#include <Arduino.h>
#include <WiFiClientSecure.h>

#ifndef ENABLE_USER_AUTH
#define ENABLE_USER_AUTH
#endif

#ifndef ENABLE_DATABASE
#define ENABLE_DATABASE
#endif

#include <FirebaseClient.h>

void processData(AsyncResult &aResult);
void firebaseStartup();
void runFirebase();
bool firebaseReady();
bool firebaseUpsertController(const String &deviceCode,
                              const String &penCode,
                              bool online,
                              uint32_t lastSeenEpoch,
                              const String &source);
bool firebaseLogEvent(const String &deviceCode,
                      const String &penCode,
                      const String &eventType,
                      const String &payload,
                      uint32_t eventEpoch);

#endif
