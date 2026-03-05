#include "rtc.h"

RTC_DS3231 rtc;
bool rtcReady = false;

const uint8_t SDA_PIN = 21;
const uint8_t SCL_PIN_PRIMARY = 22;
const uint8_t SCL_PIN_FALLBACK = 23;
unsigned long lastRtcRetryMs = 0;

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
    Serial.println("RTC unavailable, check wiring/pins/module type");
    if (millis() - lastRtcRetryMs > 10000) {
      lastRtcRetryMs = millis();
      Serial.println("Retrying RTC detection...");
      rtcStartup();
    }
    delay(1000);
    return;
  }

  DateTime now = rtc.now();
  Serial.printf("%d/%d/%d %d:%d:%d\n",
                now.year(), now.month(), now.day(),
                now.hour(), now.minute(), now.second());
  delay(1000);
}
