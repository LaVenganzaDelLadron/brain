#ifndef DEVICE_H
#define DEVICE_H

#include <Arduino.h>

bool postDeviceToApi(const String &deviceCode, const String &penCode);

#endif
