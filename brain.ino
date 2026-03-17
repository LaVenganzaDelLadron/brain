#include <core/wifi.h>
#include <core/rtc.h>
#include <config/firebase.h>

#if defined(ESP32)
#include <esp_sleep.h>
#include <esp_wifi.h>
#endif

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("System Starting...");

#if defined(ESP32)
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#endif

  startup();
  rtcStartup();
}

void loop() {
  maintainWiFiConnection();
  runControllerHub();
  runFirebase();
  getTime();
  delay(1500);
  yield();
}
