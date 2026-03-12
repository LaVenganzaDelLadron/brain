#include "rtc.h"

RTC_DS3231 rtc;
bool rtcReady = false;

const uint8_t SDA_PIN = 21;
const uint8_t SCL_PIN_PRIMARY = 22;
const uint8_t SCL_PIN_FALLBACK = 23;
unsigned long lastRtcRetryMs = 0;
unsigned long lastTimePrintMs = 0;

byte scanI2C() {
  byte count = 0;
  Serial.println("Scanning I2C...");
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("I2C device found at 0x%02X\n", address);
      count++;
    }
  }

  if (count == 0) {
    Serial.println("No I2C devices found");
  }

  return count;
}

void rtcStartup() {
  byte devices = 0;

  Serial.printf("Trying I2C SDA=%d SCL=%d\n", SDA_PIN, SCL_PIN_PRIMARY);
  Wire.begin(SDA_PIN, SCL_PIN_PRIMARY);
  devices = scanI2C();

  if (devices == 0) {
    Serial.printf("Trying I2C SDA=%d SCL=%d\n", SDA_PIN, SCL_PIN_FALLBACK);
    Wire.begin(SDA_PIN, SCL_PIN_FALLBACK);
    devices = scanI2C();
  }

  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    rtcReady = false;
    return;
  }
  rtcReady = true;
  Serial.println("RTC found");

  if (rtc.lostPower()) {
    Serial.println("RTC is NOT running, setting time!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

}


void getTime() {
  if (!rtcReady) {
    if (millis() - lastRtcRetryMs > 10000) {
      lastRtcRetryMs = millis();
      Serial.println("RTC unavailable, retrying detection...");
      rtcStartup();
    }
    return;
  }

  if (millis() - lastTimePrintMs < 1000) {
    return;
  }
  lastTimePrintMs = millis();

  DateTime now = rtc.now();
  Serial.printf("%d/%d/%d %d:%d:%d\n",
                now.year(), now.month(), now.day(),
                now.hour(), now.minute(), now.second());
}

bool rtcHasTime() {
  return rtcReady;
}

uint32_t rtcUnixTime() {
  if (!rtcReady) {
    return 0;
  }

  DateTime now = rtc.now();
  return static_cast<uint32_t>(now.unixtime());
}

bool rtcGetIsoTimestamp(String &out) {
  if (!rtcReady) {
    return false;
  }

  DateTime now = rtc.now();
  char buffer[21];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  out = buffer;
  return true;
}
