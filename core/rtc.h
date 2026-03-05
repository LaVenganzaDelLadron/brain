#ifndef RTC_H
#define RTC_H

#include <Wire.h>
#include <RTClib.h>

void rtcStartup();
void getTime();
bool rtcHasTime();
uint32_t rtcUnixTime();

#endif
