// IoTDeck: free dashboards for ESP32 projects.
// The board sends values to the dashboard and receives switch commands from it.
//
// Usage:
//   IoTDeck.begin("device-key", "WiFi-name", "WiFi-password");
//   IoTDeck.send("TEMPERATURE", temperature, 0.5, 300);  // send when it moves by 0.5, at most every 300 seconds
//   IoTDeck.send("TEMPERATURE", temperature);            // any change, at most every 300 seconds (the defaults)
//   IoTDeck.sendEvery("TEMPERATURE", temperature, 60);   // every 60 seconds, changed or not
//   IOTDECK_WRITE(LED) { digitalWrite(LED_BUILTIN, value); }  // runs when the LED switch changes
//   In setup(), before begin(): pinMode(LED_BUILTIN, OUTPUT);  // digitalWrite needs the pin set to output first
//
// Remember: every send uses one of the owner's daily sends. Sending every second uses 500 sends (the default
// daily limit) in 8 minutes 20 seconds. The server also spaces posts by its daily-limit gap, so the real rate
// at the default limit is slower than the seconds you choose.

#pragma once

// This library includes the ArduinoJson and WebSockets libraries, so only the ESP32 board package is needed.
#if !defined(ESP32)
#error "IoTDeck works on ESP32 boards only. Choose an ESP32 board (Tools > Board) after installing the 'esp32' board package by Espressif."
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <math.h>

// Some ESP32 board definitions do not define the built-in LED. Pin 2 is the usual one.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#define IOTDECK_HOST "iotdeck.org"
#define IOTDECK_HEARTBEAT_MS (15UL * 60UL * 1000UL)  // If nothing was sent for this long, the current value is sent once
#define IOTDECK_MIN_GAP_MS 1000UL                     // The only hard limit: at most one post per second
#define IOTDECK_DEFAULT_SECONDS 300UL                 // Default wait between two sends of one value: 5 minutes
#define IOTDECK_PING_MS 30000UL                       // Tells the dashboard the board is still connected
#define IOTDECK_MAX_SERIES 8                          // Values per board

// ---------------------------------------------------------------------------
// Handlers for dashboard switches, registered by the IOTDECK_WRITE macro
// ---------------------------------------------------------------------------

typedef void (*IoTDeckWriteFn)(int value);

struct IoTDeckWriteEntry {
  const char* name;
  IoTDeckWriteFn fn;
};

inline int& iotdeckWriteCount() {
  static int count = 0;
  return count;
}

inline IoTDeckWriteEntry* iotdeckWriteList() {
  static IoTDeckWriteEntry list[IOTDECK_MAX_SERIES];
  return list;
}

struct IoTDeckWriteRegistrar {
  IoTDeckWriteRegistrar(const char* name, IoTDeckWriteFn fn) {
    int& count = iotdeckWriteCount();
    if (count < IOTDECK_MAX_SERIES) {
      iotdeckWriteList()[count++] = {name, fn};
    }
  }
};

// IOTDECK_WRITE(LED) { ... } defines a handler named after the dashboard control "LED".
// Inside it, `value` is 1 when the switch is on and 0 when it is off.
#define IOTDECK_WRITE(name)                                              \
  static void iotdeck_write_##name(int value);                           \
  static IoTDeckWriteRegistrar iotdeck_reg_##name(#name, iotdeck_write_##name); \
  static void iotdeck_write_##name(int value)

// ---------------------------------------------------------------------------
// The library
// ---------------------------------------------------------------------------

class IoTDeckClass {
public:
  // Connects to Wi-Fi and to the dashboard. Call once in setup().
  bool begin(const char* key, const char* ssid, const char* pass) {
    Serial.begin(115200);
    _key = key;
    _auth = String("Authorization: Bearer ") + key;
    _mutex = xSemaphoreCreateMutex();

    Serial.printf("[IoTDeck] Connecting to Wi-Fi %s\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[IoTDeck] Wi-Fi connection failed");
      return false;
    }
    Serial.printf("[IoTDeck] Wi-Fi connected, IP %s\n", WiFi.localIP().toString().c_str());

    _ws.setExtraHeaders(_auth.c_str());
    _ws.onEvent([this](WStype_t type, uint8_t* payload, size_t length) { onSocket(type, payload, length); });
    _ws.beginSSL(IOTDECK_HOST, 443, "/ws/device");
    _ws.setReconnectInterval(5000);

    // Background task: keeps the live connection and posts readings. Runs without loop() calls.
    // Two tasks: one keeps the live connection and receives commands, the other posts readings.
    // A post can take a second or more, and it must not hold up commands.
    xTaskCreatePinnedToCore(backgroundTask, "iotdeck", 8192, this, 1, nullptr, 0);
    xTaskCreatePinnedToCore(postTask, "iotdeckPost", 16384, this, 1, nullptr, 0);
    return true;
  }

  // Sends a value when it has changed by at least `change` (any change when `change` is 0), and at least
  // `seconds` have passed since this value was last sent. If nothing has been sent for 15 minutes, the current
  // value is sent once. Defaults: any change, at most every 300 seconds. Minimum wait: 1 second.
  // Returns immediately, never blocks.
  void send(const char* name, float value, float change = 0, unsigned long seconds = IOTDECK_DEFAULT_SECONDS) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    Series* s = findOrAdd(name);
    if (s && !s->muted) {
      s->timed = false;
      s->change = change;
      s->gapMs = secondsToMs(seconds);
      bool moved = !s->hasLast || (change == 0 ? value != s->last : fabs(value - s->last) >= change);
      bool beat = s->hasLast && millis() - s->lastSentMs >= IOTDECK_HEARTBEAT_MS;
      if (moved || beat) {
        s->pendingValue = value;
        s->pending = true;
        s->beat = beat && !moved;
      }
    }
    xSemaphoreGive(_mutex);
  }

  // Sends the value every `seconds`, whether or not it changed. Minimum: 1 second.
  // Returns immediately, never blocks.
  void sendEvery(const char* name, float value, unsigned long seconds = IOTDECK_DEFAULT_SECONDS) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    Series* s = findOrAdd(name);
    if (s && !s->muted) {
      s->timed = true;
      s->beat = false;
      s->gapMs = secondsToMs(seconds);
      bool due = !s->hasLast || millis() - s->lastSentMs >= s->gapMs;
      if (due) {
        s->pendingValue = value;
        s->pending = true;
      }
    }
    xSemaphoreGive(_mutex);
  }

private:
  struct Series {
    String name;
    float last = 0;                 // The last value the server confirmed
    bool hasLast = false;
    unsigned long lastSentMs = 0;
    float pendingValue = 0;
    bool pending = false;
    bool beat = false;              // Pending only because of the 15-minute heartbeat
    bool timed = false;             // Sent by sendEvery(): every `gapMs`, changed or not
    bool muted = false;             // Switched off on the dashboard: nothing is sent until it is switched on
    float change = 0;               // The change band from the last send() call
    unsigned long gapMs = IOTDECK_DEFAULT_SECONDS * 1000UL;  // The shortest wait between two sends of this value
  };

  String _key;
  String _auth;
  WebSocketsClient _ws;
  SemaphoreHandle_t _mutex = nullptr;
  Series _series[IOTDECK_MAX_SERIES];
  int _seriesCount = 0;
  unsigned long _lastPost = 0;
  unsigned long _serverGapMs = 0;  // Shortest gap between posts the server allows for this user's daily limit
  unsigned long _lastPing = 0;

  // The user's seconds, never less than 1
  static unsigned long secondsToMs(unsigned long seconds) {
    return (seconds < 1 ? 1UL : seconds) * 1000UL;
  }

  Series* findOrAdd(const char* name) {
    for (int i = 0; i < _seriesCount; i++) {
      if (_series[i].name == name) return &_series[i];
    }
    if (_seriesCount >= IOTDECK_MAX_SERIES) return nullptr;
    _series[_seriesCount].name = name;
    return &_series[_seriesCount++];
  }

  static void backgroundTask(void* arg) {
    IoTDeckClass* self = (IoTDeckClass*)arg;
    for (;;) {
      self->_ws.loop();
      self->keepAlive();
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  // Posts readings. Kept apart from the socket so a slow post never delays a command.
  static void postTask(void* arg) {
    IoTDeckClass* self = (IoTDeckClass*)arg;
    for (;;) {
      self->postPending();
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  // Tells the dashboard the board is still there. A board that vanishes is treated as gone after 3 minutes.
  void keepAlive() {
    if (!_ws.isConnected()) return;
    if (millis() - _lastPing < IOTDECK_PING_MS) return;
    _lastPing = millis();
    _ws.sendTXT("ping");
  }

  // The reply lists the fields the dashboard has switched off. The board stops sending those.
  void applyMuted(JsonDocument& reply) {
    for (int i = 0; i < _seriesCount; i++) _series[i].muted = false;
    JsonArray list = reply["muted"].as<JsonArray>();
    for (JsonVariant item : list) {
      const char* name = item.as<const char*>();
      if (!name) continue;
      for (int i = 0; i < _seriesCount; i++) {
        if (_series[i].name == name) _series[i].muted = true;
      }
    }
  }

  // Posts every value that is due in one request. The gap between posts is at least the server's limit gap.
  void postPending() {
    if (WiFi.status() != WL_CONNECTED) return;
    unsigned long gap = _serverGapMs > IOTDECK_MIN_GAP_MS ? _serverGapMs : IOTDECK_MIN_GAP_MS;
    if (millis() - _lastPost < gap && _lastPost != 0) return;

    // Copy what is due while holding the lock. We remember exactly what we sent.
    JsonDocument body;
    float sentValue[IOTDECK_MAX_SERIES];
    bool sentNow[IOTDECK_MAX_SERIES] = {false};
    bool any = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (int i = 0; i < _seriesCount; i++) {
      Series& s = _series[i];
      if (s.muted) {
        s.pending = false;
        continue;
      }
      if (!s.pending) continue;
      // A change-based value that has come back to the last value sent is no longer a change
      if (!s.timed && !s.beat && s.hasLast) {
        bool moved = s.change == 0 ? s.pendingValue != s.last : fabs(s.pendingValue - s.last) >= s.change;
        if (!moved) {
          s.pending = false;
          continue;
        }
      }
      // Each value waits its own gap after its last send. It stays pending until then.
      if (s.hasLast && millis() - s.lastSentMs < s.gapMs) continue;
      sentValue[i] = s.pendingValue;
      sentNow[i] = true;
      body[s.name] = sentValue[i];
      any = true;
    }
    xSemaphoreGive(_mutex);
    if (!any) return;

    _lastPost = millis();
    String text;
    serializeJson(body, text);

    WiFiClientSecure secure;
    secure.setInsecure();  // Encrypted, not verified. Add a CA certificate for production.
    HTTPClient http;
    http.begin(secure, String("https://") + IOTDECK_HOST + "/ingest");
    http.setTimeout(10000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + _key);
    int code = http.POST(text);
    JsonDocument reply;
    if (code == 200 || code == 409) {
      // The reply says how long to wait between posts. After a refused send it says how long to pause.
      if (!deserializeJson(reply, http.getString())) _serverGapMs = reply["minGapMs"] | 0UL;
    }
    http.end();

    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (code == 200) applyMuted(reply);
    for (int i = 0; i < _seriesCount; i++) {
      if (!sentNow[i]) continue;
      if (code == 200) {
        _series[i].last = sentValue[i];
        _series[i].hasLast = true;
        _series[i].lastSentMs = millis();
        // A newer value may have arrived during the post. Keep it pending only if it is still a new value.
        bool same = _series[i].pendingValue == sentValue[i];
        bool within = !_series[i].timed && fabs(_series[i].pendingValue - sentValue[i]) < _series[i].change;
        if (same || within) {
          _series[i].pending = false;
          _series[i].beat = false;
        }
        Serial.printf("[IoTDeck] Sent %s = %.2f\n", _series[i].name.c_str(), sentValue[i]);
      } else if (code == 409) {
        // No gadget shows this value any more. Drop it, and do not retry.
        _series[i].pending = false;
        Serial.printf("[IoTDeck] Not sent: no gadget shows %s. Add one on the dashboard.\n", _series[i].name.c_str());
      }
    }
    xSemaphoreGive(_mutex);
    if (code != 200 && code != 409) {
      // Negative codes are network errors (no reply from the server). The value is kept and retried.
      Serial.printf("[IoTDeck] Send failed (%d): %s. It will be retried.\n", code, HTTPClient::errorToString(code).c_str());
    }
  }

  // Live socket: receives switch commands from the dashboard
  void onSocket(WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_CONNECTED) {
      Serial.println("[IoTDeck] Connected to the dashboard, waiting for commands");
      // Tell the dashboard which switches this sketch handles
      JsonDocument hello;
      hello["type"] = "hello";
      JsonArray names = hello["handlers"].to<JsonArray>();
      int count = iotdeckWriteCount();
      IoTDeckWriteEntry* list = iotdeckWriteList();
      for (int i = 0; i < count; i++) names.add(list[i].name);
      String text;
      serializeJson(hello, text);
      _ws.sendTXT(text);
      return;
    }
    if (type == WStype_DISCONNECTED) {
      Serial.println("[IoTDeck] Dashboard connection lost, reconnecting");
      return;
    }
    if (type != WStype_TEXT) return;
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) return;
    // The dashboard switched a value off or on
    if (doc["type"] == "mute") {
      const char* field = doc["field"] | "";
      bool muted = doc["muted"] | false;
      for (int i = 0; i < _seriesCount; i++) {
        if (_series[i].name == field) _series[i].muted = muted;
      }
      Serial.printf("[IoTDeck] %s is %s on the dashboard\n", field, muted ? "switched off" : "switched on");
      return;
    }
    // The dashboard added or changed a widget, so a paused board can send again now
    if (doc["type"] == "resume") {
      _serverGapMs = 0;
      Serial.println("[IoTDeck] A widget was added on the dashboard. Sending again.");
      return;
    }
    if (doc["type"] != "command") return;

    const char* field = doc["field"] | "";
    JsonVariant value = doc["value"];
    int v = value.is<bool>() ? (value.as<bool>() ? 1 : 0) : value.as<int>();
    // Switches print ON or OFF. Sliders and other numbers print the number itself, as the handler receives it.
    if (value.is<bool>()) {
      Serial.printf("[IoTDeck] Command %s = %s\n", field, v ? "ON (1)" : "OFF (0)");
    } else {
      Serial.printf("[IoTDeck] Command %s = %d\n", field, v);
    }

    int count = iotdeckWriteCount();
    IoTDeckWriteEntry* list = iotdeckWriteList();
    bool found = false;
    for (int i = 0; i < count; i++) {
      if (strcmp(list[i].name, field) == 0) {
        list[i].fn(v);
        found = true;
      }
    }
    if (!found) {
      Serial.printf("[IoTDeck] No handler named %s. Add IOTDECK_WRITE(%s) to your sketch.\n", field, field);
      return;
    }
    // Confirm to the dashboard that the board applied the value. The dashboard shows this, not what it sent.
    JsonDocument ack;
    ack["type"] = "ack";
    ack["field"] = field;
    ack["value"] = v;
    String text;
    serializeJson(ack, text);
    _ws.sendTXT(text);
  }
};

static IoTDeckClass IoTDeck;
