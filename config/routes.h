#ifndef ROUTES_H
#define ROUTES_H

#include <Arduino.h>

// Base URL (global constant)
const String BASE_URL = "https://shapi-qq0p.onrender.com";

String endpointUrl(String path) {
  String baseUrl = BASE_URL;
  if (baseUrl.endsWith("/")) {
    baseUrl.remove(baseUrl.length() - 1);
  }
  if (!path.startsWith("/")) {
    path = "/" + path;
  }
  return baseUrl + path;
}

const String PG_TELEMETRY = endpointUrl("api/telemetry");
const String PG_STATUS = endpointUrl("api/status");

#endif