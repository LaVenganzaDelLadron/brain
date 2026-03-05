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

#endif
