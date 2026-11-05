/*
  IoTDeck: free dashboards for ESP32 projects.

  Test write: the dashboard's LED switch turns the board's built-in LED on or off.
  Fill in the Wi-Fi name and password, then upload the sketch.
*/

#include <IoTDeck.h>                                  // The IoTDeck library

// Runs when the dashboard's LED switch changes. value is 1 when on, 0 when off.
IOTDECK_WRITE(LED) {
  digitalWrite(LED_BUILTIN, value);
}

void setup() {
  // The LED pin must be an output before digitalWrite can change it.
  pinMode(LED_BUILTIN, OUTPUT);
  // Connects to Wi-Fi and to the dashboard.
  IoTDeck.begin("your-device-key", "Wi-Fi name", "Wi-Fi password");
}

void loop() {
  // Nothing to do here. The library works in the background.
}
