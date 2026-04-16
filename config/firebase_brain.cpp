#include "firebase_internal.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <strings.h>
#include <time.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

namespace {

const unsigned long BRAIN_POLL_INTERVAL_MS = 5000;
const unsigned long BRAIN_HEARTBEAT_TIMEOUT_SEC = 20;
const size_t BRAIN_LEDGER_LEN = 48;
const size_t BRAIN_DEVICE_LIMIT = 64;
const size_t BRAIN_SCHEDULE_LIMIT = 128;

struct BrainDeviceStatus {
  String deviceCode;
  String penCode;
  bool online = false;
  uint32_t lastSeenEpoch = 0;
  bool exists = false;
};

struct BrainSchedule {
  String scheduleId;
  String deviceCode;
  String penCode;
  String growthCode;
  String batchCode;
  String feedCode;
  float feedQuantity = 0.0f;
  String feedTime;
  bool enabled = false;
  bool repeatEveryday = false;
  uint8_t repeatDayMask = 0;
  int16_t feedMinuteOfDay = -1;
};

struct BrainCommandLedgerEntry {
  String scheduleId;
  String deviceCode;
  uint32_t minuteBucket = 0;
};

unsigned long lastBrainPollMs = 0;
BrainCommandLedgerEntry brainLedger[BRAIN_LEDGER_LEN];
size_t brainLedgerWriteIndex = 0;

uint32_t brainCurrentEpoch() {
  const time_t nowEpoch = time(nullptr);
  if (nowEpoch > 1700000000UL) {
    return static_cast<uint32_t>(nowEpoch);
  }
  return 1700000000UL + static_cast<uint32_t>(millis() / 1000UL);
}

bool brainReadBool(JsonVariantConst value, bool fallback) {
  if (value.is<bool>()) {
    return value.as<bool>();
  }

  const char *text = value.as<const char *>();
  if (!text) {
    return fallback;
  }

  if (strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0 || strcasecmp(text, "yes") == 0) {
    return true;
  }
  if (strcasecmp(text, "false") == 0 || strcmp(text, "0") == 0 || strcasecmp(text, "no") == 0) {
    return false;
  }
  return fallback;
}

int16_t parseFeedMinuteOfDay(const String &feedTime) {
  if (feedTime.length() != 5 || feedTime.charAt(2) != ':') {
    return -1;
  }

  if (!isdigit(static_cast<unsigned char>(feedTime.charAt(0))) ||
      !isdigit(static_cast<unsigned char>(feedTime.charAt(1))) ||
      !isdigit(static_cast<unsigned char>(feedTime.charAt(3))) ||
      !isdigit(static_cast<unsigned char>(feedTime.charAt(4)))) {
    return -1;
  }

  const int hour = (feedTime.charAt(0) - '0') * 10 + (feedTime.charAt(1) - '0');
  const int minute = (feedTime.charAt(3) - '0') * 10 + (feedTime.charAt(4) - '0');
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return -1;
  }

  return static_cast<int16_t>(hour * 60 + minute);
}

uint8_t weekdayBitFromName(const char *name) {
  if (!name) {
    return 0;
  }
  if (strcasecmp(name, "sunday") == 0 || strcasecmp(name, "sun") == 0) return 1U << 0;
  if (strcasecmp(name, "monday") == 0 || strcasecmp(name, "mon") == 0) return 1U << 1;
  if (strcasecmp(name, "tuesday") == 0 || strcasecmp(name, "tue") == 0 || strcasecmp(name, "tues") == 0) return 1U << 2;
  if (strcasecmp(name, "wednesday") == 0 || strcasecmp(name, "wed") == 0) return 1U << 3;
  if (strcasecmp(name, "thursday") == 0 || strcasecmp(name, "thu") == 0 || strcasecmp(name, "thurs") == 0) return 1U << 4;
  if (strcasecmp(name, "friday") == 0 || strcasecmp(name, "fri") == 0) return 1U << 5;
  if (strcasecmp(name, "saturday") == 0 || strcasecmp(name, "sat") == 0) return 1U << 6;
  return 0;
}

uint8_t parseRepeatDayMask(JsonVariantConst value) {
  if (value.is<JsonArrayConst>()) {
    uint8_t mask = 0;
    JsonArrayConst list = value.as<JsonArrayConst>();
    for (JsonVariantConst entry : list) {
      mask |= weekdayBitFromName(entry.as<const char *>());
    }
    return mask;
  }

  const char *text = value.as<const char *>();
  if (!text) {
    return 0;
  }

  String raw(text);
  raw.trim();
  if (raw.length() == 0) {
    return 0;
  }

  uint8_t mask = 0;
  int start = 0;
  while (start < static_cast<int>(raw.length())) {
    int comma = raw.indexOf(',', start);
    if (comma < 0) {
      comma = raw.length();
    }

    String token = raw.substring(start, comma);
    token.trim();
    if (token.length() > 0) {
      mask |= weekdayBitFromName(token.c_str());
    }

    start = comma + 1;
  }

  return mask;
}

bool ledgerHasRecentCommand(const String &scheduleId, const String &deviceCode, uint32_t minuteBucket) {
  for (size_t i = 0; i < BRAIN_LEDGER_LEN; ++i) {
    if (brainLedger[i].minuteBucket == minuteBucket &&
        brainLedger[i].scheduleId == scheduleId &&
        brainLedger[i].deviceCode == deviceCode) {
      return true;
    }
  }
  return false;
}

void ledgerRememberCommand(const String &scheduleId, const String &deviceCode, uint32_t minuteBucket) {
  brainLedger[brainLedgerWriteIndex].scheduleId = scheduleId;
  brainLedger[brainLedgerWriteIndex].deviceCode = deviceCode;
  brainLedger[brainLedgerWriteIndex].minuteBucket = minuteBucket;
  brainLedgerWriteIndex = (brainLedgerWriteIndex + 1) % BRAIN_LEDGER_LEN;
}

bool schedulerGetJson(const String &path, String *payloadOut) {
  if (!payloadOut) {
    return false;
  }

  String payload = SchedulerDatabase.get<String>(schedulerClient, path);
  if (schedulerClient.lastError().code() != 0) {
    Serial.printf("Brain read failed at %s: %s (code=%d)\n",
                  path.c_str(),
                  schedulerClient.lastError().message().c_str(),
                  schedulerClient.lastError().code());
    return false;
  }

  *payloadOut = payload;
  return true;
}

bool parseActiveDevices(const String &payload, String activeDevices[], size_t *countOut, bool *hasFilterOut) {
  if (!countOut || !hasFilterOut) {
    return false;
  }

  *countOut = 0;
  *hasFilterOut = false;

  if (payload.length() == 0 || payload == "null") {
    return true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Failed to parse /devices payload: %s\n", err.c_str());
    return false;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    return false;
  }

  for (JsonPairConst entry : root) {
    if (*countOut >= BRAIN_DEVICE_LIMIT) {
      break;
    }

    JsonObjectConst obj = entry.value().as<JsonObjectConst>();
    if (obj.isNull()) {
      continue;
    }

    const bool enabled = brainReadBool(obj["enabled"], true) && brainReadBool(obj["active"], true);
    if (!enabled) {
      continue;
    }

    String deviceCode = String(obj["device_code"] | entry.key().c_str());
    deviceCode.trim();
    if (deviceCode.length() == 0) {
      continue;
    }

    activeDevices[*countOut] = deviceCode;
    (*countOut)++;
    *hasFilterOut = true;
  }

  return true;
}

bool parseControllers(const String &payload, BrainDeviceStatus controllers[], size_t *countOut) {
  if (!countOut) {
    return false;
  }
  *countOut = 0;

  if (payload.length() == 0 || payload == "null") {
    return true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Failed to parse /controllers payload: %s\n", err.c_str());
    return false;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    return false;
  }

  for (JsonPairConst entry : root) {
    if (*countOut >= BRAIN_DEVICE_LIMIT) {
      break;
    }

    JsonObjectConst obj = entry.value().as<JsonObjectConst>();
    if (obj.isNull()) {
      continue;
    }

    BrainDeviceStatus status;
    status.deviceCode = String(obj["device_code"] | entry.key().c_str());
    status.penCode = String(obj["pen_code"] | "");
    status.online = brainReadBool(obj["online"], false);
    status.lastSeenEpoch = obj["last_seen_epoch"] | 0;
    status.exists = status.deviceCode.length() > 0;
    if (!status.exists) {
      continue;
    }

    controllers[*countOut] = status;
    (*countOut)++;
  }

  return true;
}

BrainDeviceStatus *findController(BrainDeviceStatus controllers[], size_t count, const String &deviceCode) {
  for (size_t i = 0; i < count; ++i) {
    if (controllers[i].deviceCode == deviceCode) {
      return &controllers[i];
    }
  }
  return nullptr;
}

bool deviceIsActive(const String &deviceCode, const String activeDevices[], size_t activeCount, bool hasFilter) {
  if (!hasFilter) {
    return true;
  }

  for (size_t i = 0; i < activeCount; ++i) {
    if (activeDevices[i] == deviceCode) {
      return true;
    }
  }
  return false;
}

bool parseSchedules(const String &payload, BrainSchedule schedules[], size_t *countOut) {
  if (!countOut) {
    return false;
  }
  *countOut = 0;

  if (payload.length() == 0 || payload == "null") {
    return true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Failed to parse /feeding_schedules payload: %s\n", err.c_str());
    return false;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    return false;
  }

  for (JsonPairConst entry : root) {
    if (*countOut >= BRAIN_SCHEDULE_LIMIT) {
      break;
    }

    JsonObjectConst obj = entry.value().as<JsonObjectConst>();
    if (obj.isNull()) {
      continue;
    }

    BrainSchedule schedule;
    schedule.scheduleId = entry.key().c_str();
    schedule.deviceCode = String(obj["device_code"] | "");
    schedule.penCode = String(obj["pen_code"] | "");
    schedule.growthCode = String(obj["growth_code"] | "");
    schedule.batchCode = String(obj["batch_code"] | "");
    schedule.feedCode = String(obj["feed_code"] | schedule.scheduleId);
    schedule.feedQuantity = obj["feed_quantity"] | 0.0f;
    schedule.feedTime = String(obj["feed_time"] | "");
    schedule.enabled = brainReadBool(obj["enabled"], false);
    schedule.repeatEveryday = strcasecmp(String(obj["repeat_days"] | "").c_str(), "everyday") == 0;
    schedule.repeatDayMask = parseRepeatDayMask(obj["repeat_days_list"]);
    schedule.feedMinuteOfDay = parseFeedMinuteOfDay(schedule.feedTime);

    if (!schedule.enabled ||
        schedule.deviceCode.length() == 0 ||
        schedule.growthCode.length() == 0 ||
        schedule.feedMinuteOfDay < 0) {
      continue;
    }

    schedules[*countOut] = schedule;
    (*countOut)++;
  }

  return true;
}

bool schedulerWriteJson(const String &path, JsonDocument &doc, bool overwrite) {
  String json;
  serializeJson(doc, json);
  const bool ok = overwrite
                    ? SchedulerDatabase.set<object_t>(schedulerClient, path, object_t(json))
                    : SchedulerDatabase.update<object_t>(schedulerClient, path, object_t(json));
  if (!ok) {
    Serial.printf("Brain write failed at %s: %s (code=%d)\n",
                  path.c_str(),
                  schedulerClient.lastError().message().c_str(),
                  schedulerClient.lastError().code());
  }
  return ok;
}

bool schedulerHasPendingCommand(const String &deviceCode) {
  String payload;
  if (!schedulerGetJson("/controller_commands/" + deviceCode, &payload)) {
    return true;
  }
  if (payload.length() == 0 || payload == "null") {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    return true;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    return true;
  }
  return brainReadBool(root["trigger"], false);
}

bool publishControllerCommand(const BrainSchedule &schedule, uint32_t nowEpoch) {
  if (schedulerHasPendingCommand(schedule.deviceCode)) {
    return false;
  }

  JsonDocument commandDoc;
  const String commandId = schedule.feedCode + "_" + String(static_cast<unsigned long>(nowEpoch));
  commandDoc["command_id"] = commandId;
  commandDoc["device_code"] = schedule.deviceCode;
  commandDoc["feed_code"] = schedule.feedCode;
  commandDoc["growth_code"] = schedule.growthCode;
  commandDoc["feed_quantity"] = schedule.feedQuantity;
  commandDoc["trigger"] = true;
  commandDoc["command_epoch"] = nowEpoch;
  commandDoc["pen_code"] = schedule.penCode;
  commandDoc["batch_code"] = schedule.batchCode;
  commandDoc["execution_status"] = "pending";

  const String path = "/controller_commands/" + schedule.deviceCode;
  if (!schedulerWriteJson(path, commandDoc, true)) {
    return false;
  }

  Serial.printf("Published command %s for %s (%s)\n",
                commandId.c_str(),
                schedule.deviceCode.c_str(),
                schedule.growthCode.c_str());
  return true;
}

void reconcileControllerFreshness(BrainDeviceStatus controllers[], size_t controllerCount, uint32_t nowEpoch) {
  for (size_t i = 0; i < controllerCount; ++i) {
    BrainDeviceStatus &ctrl = controllers[i];
    if (ctrl.deviceCode.length() == 0 || ctrl.penCode.length() == 0) {
      continue;
    }

    const bool fresh = ctrl.lastSeenEpoch > 0 &&
                       nowEpoch >= ctrl.lastSeenEpoch &&
                       (nowEpoch - ctrl.lastSeenEpoch) <= BRAIN_HEARTBEAT_TIMEOUT_SEC;

    if (ctrl.online && !fresh) {
      uint32_t requestId = 0;
      const bool queued = firebaseQueueUpsertController(ctrl.deviceCode,
                                                        ctrl.penCode,
                                                        false,
                                                        nowEpoch,
                                                        "brain_timeout",
                                                        &requestId);
      if (queued) {
        ctrl.online = false;
      }
    }
  }
}

}  // namespace

void firebaseRunBrainLoop() {
  if (!firebaseReady() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  const unsigned long nowMs = millis();
  if (nowMs - lastBrainPollMs < BRAIN_POLL_INTERVAL_MS) {
    return;
  }
  lastBrainPollMs = nowMs;

  String controllersPayload;
  String devicesPayload;
  String schedulesPayload;
  if (!schedulerGetJson("/controllers", &controllersPayload)) {
    return;
  }
  if (!schedulerGetJson("/devices", &devicesPayload)) {
    return;
  }
  if (!schedulerGetJson("/feeding_schedules", &schedulesPayload)) {
    return;
  }

  String activeDevices[BRAIN_DEVICE_LIMIT];
  size_t activeDeviceCount = 0;
  bool hasDeviceFilter = false;
  if (!parseActiveDevices(devicesPayload, activeDevices, &activeDeviceCount, &hasDeviceFilter)) {
    return;
  }

  BrainDeviceStatus controllers[BRAIN_DEVICE_LIMIT];
  size_t controllerCount = 0;
  if (!parseControllers(controllersPayload, controllers, &controllerCount)) {
    return;
  }

  const uint32_t nowEpoch = brainCurrentEpoch();
  reconcileControllerFreshness(controllers, controllerCount, nowEpoch);

  BrainSchedule schedules[BRAIN_SCHEDULE_LIMIT];
  size_t scheduleCount = 0;
  if (!parseSchedules(schedulesPayload, schedules, &scheduleCount)) {
    return;
  }

  time_t nowTime = static_cast<time_t>(nowEpoch);
  struct tm localNow;
  localtime_r(&nowTime, &localNow);
  const int currentMinute = localNow.tm_hour * 60 + localNow.tm_min;
  const uint8_t weekdayBit = (localNow.tm_wday >= 0 && localNow.tm_wday <= 6) ? (1U << localNow.tm_wday) : 0;
  const uint32_t minuteBucket = nowEpoch / 60U;

  for (size_t i = 0; i < scheduleCount; ++i) {
    const BrainSchedule &schedule = schedules[i];
    if (schedule.feedMinuteOfDay != currentMinute) {
      continue;
    }

    if (!schedule.repeatEveryday && (schedule.repeatDayMask == 0 || (schedule.repeatDayMask & weekdayBit) == 0)) {
      continue;
    }

    if (!deviceIsActive(schedule.deviceCode, activeDevices, activeDeviceCount, hasDeviceFilter)) {
      continue;
    }

    BrainDeviceStatus *controller = findController(controllers, controllerCount, schedule.deviceCode);
    if (!controller) {
      continue;
    }

    const bool fresh = controller->lastSeenEpoch > 0 &&
                       nowEpoch >= controller->lastSeenEpoch &&
                       (nowEpoch - controller->lastSeenEpoch) <= BRAIN_HEARTBEAT_TIMEOUT_SEC;
    if (!controller->online || !fresh) {
      continue;
    }

    if (ledgerHasRecentCommand(schedule.scheduleId, schedule.deviceCode, minuteBucket)) {
      continue;
    }

    if (publishControllerCommand(schedule, nowEpoch)) {
      ledgerRememberCommand(schedule.scheduleId, schedule.deviceCode, minuteBucket);
      break;
    }
  }
}
