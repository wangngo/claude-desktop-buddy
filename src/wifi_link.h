#pragma once
// WiFi transport: STA + TCP server (port 47831) speaking the same
// newline-delimited JSON as USB/BLE. mDNS name "claude-xxxx.local".
//
// Auth: on connect the stick sends {"hello":1,"nonce":"<32 hex>"}; the client
// must answer {"cmd":"auth","mac":"<hex HMAC-SHA256(key, nonce)>"} before any
// other line is accepted. The key is provisioned over USB/BLE together with
// the WiFi credentials: {"cmd":"wifi_set","ssid":..,"psk":..,"key":"<hex>"}.
// NOTE: traffic after auth is NOT encrypted (anyone on the same WLAN can
// read it); auth only stops others from driving the stick.
#include <Arduino.h>
#include <ArduinoJson.h>

inline void   wifiInit();                       // call once in setup (after settingsLoad)
inline void   wifiTick();                       // call every loop
inline bool   wifiPollLine(char* out, size_t cap);   // next authenticated line, if any
inline size_t wifiWrite(const uint8_t* data, size_t len);
inline bool   wifiCommand(JsonDocument& doc);   // wifi_set / wifi_status / wifi_off
inline bool   wifiClientActive();
inline const char* wifiStateName();
