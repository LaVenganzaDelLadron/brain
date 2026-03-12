#ifndef ROUTES_H
#define ROUTES_H

#include <Arduino.h>

// Base URL (global constant)
inline const String BASE_URL = "https://shapi-m80t.onrender.com";

inline String endpointUrl(String path) {
  String baseUrl = BASE_URL;
  if (baseUrl.endsWith("/")) {
    baseUrl.remove(baseUrl.length() - 1);
  }
  if (!path.startsWith("/")) {
    path = "/" + path;
  }
  return baseUrl + path;
}

inline const String PG_TELEMETRY = endpointUrl("api/telemetry");
inline const String PG_STATUS = endpointUrl("api/status");
inline const String PG_CONTROLLER_REGISTER = endpointUrl("api/controllers/register");
inline const String PG_CONTROLLER_HEARTBEAT = endpointUrl("api/controllers/heartbeat");
inline const String PG_COMMAND_EVENT = endpointUrl("api/controllers/command-event");

#endif
