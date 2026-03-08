#include <core/wifi.h>
#include <core/rtc.h>
#include <config/firebase.h>


void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("System Starting...");
  startup();
  rtcStartup();
}

void loop() {
  runControllerHub();
  getTime();
  runFirebase();
  delay(50);
}
