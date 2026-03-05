#include <core/wifi.h>
#include <core/rtc.h>
#include <config/firebase.h>


void setup() {
  Serial.begin(115200);
  startup();
  rtcStartup();
  firebaseStartup();
}

void loop() {
  getTime();
  runFirebase();
}
