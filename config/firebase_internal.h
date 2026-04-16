#ifndef FIREBASE_INTERNAL_H
#define FIREBASE_INTERNAL_H

#include "firebase.h"

using AsyncClient = AsyncClientClass;

extern UserAuth user_auth;

extern FirebaseApp app;
extern WiFiClientSecure ssl_client;
extern AsyncClient aClient;
extern RealtimeDatabase Database;
extern WiFiClientSecure scheduler_ssl_client;
extern AsyncClient schedulerClient;
extern RealtimeDatabase SchedulerDatabase;

void firebaseRunBrainLoop();

#endif
