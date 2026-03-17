#include "device.h"
#include "../config/routes.h"
#include "../core/rtc.h"
#include <ArduinoJson.h>
#include <new>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#else
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#endif

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

struct DevicePostItem {
  uint32_t id = 0;
  String deviceCode;
  String penCode;
};

struct DevicePostResult {
  uint32_t id = 0;
  bool success = false;
};

static const size_t DEVICE_QUEUE_LEN = 8;
static const size_t DEVICE_RESULT_LEN = 8;
static uint32_t deviceNextRequestId = 1;

#if defined(ESP32)
static QueueHandle_t deviceQueue = nullptr;
static TaskHandle_t deviceTaskHandle = nullptr;
#endif

static DevicePostResult deviceResults[DEVICE_RESULT_LEN];
static size_t deviceResultHead = 0;
static size_t deviceResultCount = 0;
#if defined(ESP32)
static portMUX_TYPE deviceResultMux = portMUX_INITIALIZER_UNLOCKED;
#endif

static void deviceRecordResult(uint32_t id, bool success) {
#if defined(ESP32)
  portENTER_CRITICAL(&deviceResultMux);
#endif
  deviceResults[deviceResultHead] = {id, success};
  deviceResultHead = (deviceResultHead + 1) % DEVICE_RESULT_LEN;
  if (deviceResultCount < DEVICE_RESULT_LEN) {
    deviceResultCount++;
  }
#if defined(ESP32)
  portEXIT_CRITICAL(&deviceResultMux);
#endif
}

static bool deviceFindResult(uint32_t requestId, bool *successOut) {
  bool found = false;
#if defined(ESP32)
  portENTER_CRITICAL(&deviceResultMux);
#endif
  for (size_t i = 0; i < deviceResultCount; ++i) {
    const size_t idx = (deviceResultHead + DEVICE_RESULT_LEN - 1 - i) % DEVICE_RESULT_LEN;
    if (deviceResults[idx].id == requestId) {
      if (successOut) {
        *successOut = deviceResults[idx].success;
      }
      found = true;
      break;
    }
  }
#if defined(ESP32)
  portEXIT_CRITICAL(&deviceResultMux);
#endif
  return found;
}

#if defined(ESP32)
static bool deviceQueuePush(DevicePostItem *item) {
  if (!deviceQueue) {
    deviceQueue = xQueueCreate(DEVICE_QUEUE_LEN, sizeof(DevicePostItem *));
    if (!deviceQueue) {
      return false;
    }
  }
  return xQueueSend(deviceQueue, &item, 0) == pdTRUE;
}

static DevicePostItem *deviceQueuePop(uint32_t timeoutMs) {
  if (!deviceQueue) {
    return nullptr;
  }
  DevicePostItem *item = nullptr;
  if (xQueueReceive(deviceQueue, &item, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
    return item;
  }
  return nullptr;
}

static void deviceTaskMain(void *param) {
  (void)param;
  const TickType_t idleDelay = pdMS_TO_TICKS(20);
  while (true) {
    DevicePostItem *item = deviceQueuePop(50);
    if (!item) {
      vTaskDelay(idleDelay);
      continue;
    }

    const bool ok = postDeviceToApi(item->deviceCode, item->penCode);
    deviceRecordResult(item->id, ok);
    delete item;
  }
}

static void deviceStartWorkerIfNeeded() {
  if (deviceTaskHandle) {
    return;
  }
  if (!deviceQueue) {
    deviceQueue = xQueueCreate(DEVICE_QUEUE_LEN, sizeof(DevicePostItem *));
    if (!deviceQueue) {
      return;
    }
  }
  xTaskCreatePinnedToCore(deviceTaskMain,
                          "deviceTask",
                          6144,
                          nullptr,
                          1,
                          &deviceTaskHandle,
                          1);
}
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
  https.setTimeout(5000);

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

bool queueDevicePost(const String &deviceCode, const String &penCode, uint32_t *requestIdOut) {
#if defined(ESP32)
  if (deviceCode.length() == 0 || penCode.length() == 0) {
    return false;
  }

  DevicePostItem *item = new (std::nothrow) DevicePostItem();
  if (!item) {
    return false;
  }

  item->id = deviceNextRequestId++;
  item->deviceCode = deviceCode;
  item->penCode = penCode;

  if (!deviceQueuePush(item)) {
    delete item;
    return false;
  }

  if (requestIdOut) {
    *requestIdOut = item->id;
  }
  deviceStartWorkerIfNeeded();
  return true;
#else
  const uint32_t id = deviceNextRequestId++;
  const bool ok = postDeviceToApi(deviceCode, penCode);
  deviceRecordResult(id, ok);
  if (requestIdOut) {
    *requestIdOut = id;
  }
  return ok;
#endif
}

bool devicePostResult(uint32_t requestId, bool *successOut) {
  return deviceFindResult(requestId, successOut);
}
