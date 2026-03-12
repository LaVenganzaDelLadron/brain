#include "device.h"
#include "../config/routes.h"
#include "../core/rtc.h"
#include <ArduinoJson.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#else
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#endif

bool postDeviceToApi(const String &deviceCode, const String &penCode) {
  if (deviceCode.length() == 0 || penCode.length() == 0) {
    Serial.println("Device post aborted: missing device_code or pen_code");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Device post aborted: Wi-Fi not connected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  const String url = endpointUrl("/device/add/");
  if (!https.begin(client, url)) {
    Serial.println("Device post failed: HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  StaticJsonDocument<192> doc;
  doc["device_code"] = deviceCode;
  doc["pen_code"] = penCode;

  String isoTimestamp;
  if (rtcGetIsoTimestamp(isoTimestamp)) {
    doc["date"] = isoTimestamp;
  } else {
    Serial.println("RTC time unavailable; sending payload without date");
  }

  String payload;
  serializeJson(doc, payload);

  const int httpCode = https.POST(payload);
  if (httpCode <= 0) {
    Serial.printf("Device post failed: %s\n", https.errorToString(httpCode).c_str());
    https.end();
    return false;
  }

  Serial.printf("Device post response code: %d\n", httpCode);
  const String responseBody = https.getString();
  Serial.println("Device post response body:");
  Serial.println(responseBody);

  https.end();
  return httpCode == 201;
}
