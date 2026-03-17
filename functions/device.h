#ifndef DEVICE_H
#define DEVICE_H

#include <Arduino.h>

bool postDeviceToApi(const String &deviceCode, const String &penCode);
bool queueDevicePost(const String &deviceCode, const String &penCode, uint32_t *requestIdOut);
bool devicePostResult(uint32_t requestId, bool *successOut);

#endif
