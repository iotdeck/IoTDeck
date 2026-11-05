/*
  IoTDeck: free dashboards for ESP32 projects.

  Test send: the board sends a temperature only when it moves by 0.5 °C, at most every 5 minutes.
  Fill in the Wi-Fi name and password, then upload the sketch.
*/

#include <IoTDeck.h>                                  // The IoTDeck library

void setup() {
  // Connects to Wi-Fi and to the dashboard.
  IoTDeck.begin("your-device-key", "Wi-Fi name", "Wi-Fi password");
}

void loop() {
  // Simulated slow drift between 20.5 and 24.5 °C. Replace with your sensor reading.
  float temperature = 22.5 + 2.0 * sin(millis() / 300000.0);

  // Sends when the value has moved by at least 0.5 °C, at most every 300 seconds (5 minutes).
  // Use IoTDeck.sendEvery("TEMPERATURE", temperature, 60) to send every 60 seconds whatever the value does.
  IoTDeck.send("TEMPERATURE", temperature, 0.5, 300);
}
