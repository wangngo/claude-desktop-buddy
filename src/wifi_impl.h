#pragma once
// Implementation of wifi_link.h. Header-only: included once from xfer.h so it
// shares the single settings() instance (stats.h state is per-TU static).
#include "wifi_link.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <mbedtls/md.h>
#include <esp_random.h>
#include "stats.h"
#include "ble_bridge.h"

static void _wifiAck(const char* what, bool ok, const char* extra = nullptr) {
  char b[200];
  int n = snprintf(b, sizeof(b), "{\"ack\":\"%s\",\"ok\":%s,\"n\":0%s}\n", what, ok ? "true" : "false", extra ? extra : "");
  Serial.write(b, n); bleWrite((const uint8_t*)b, n); wifiWrite((const uint8_t*)b, n);
}

static const uint16_t PORT = 47831;
static WiFiServer _srv(PORT);
static WiFiClient _cli;
static bool     _authed = false, _started = false, _srvUp = false;
static uint8_t  _key[32]; static size_t _keyLen = 0;
static char     _ssid[33] = "", _psk[65] = "";
static char     _nonce[33] = "";
static char     _host[24] = "";
static char     _line[1024]; static size_t _lineLen = 0;
static uint32_t _authDeadline = 0, _lastTry = 0;

static int _hex2bin(const char* h, uint8_t* out, size_t cap) {
  size_t n = strlen(h);
  if (n % 2 || n / 2 > cap) return -1;
  for (size_t i = 0; i < n / 2; i++) {
    char b[3] = {h[2 * i], h[2 * i + 1], 0};
    char* e; long v = strtol(b, &e, 16);
    if (*e) return -1;
    out[i] = (uint8_t)v;
  }
  return (int)(n / 2);
}

static void _load() {
  Preferences p; p.begin("wifi", true);
  p.getString("ssid", _ssid, sizeof(_ssid));
  p.getString("psk", _psk, sizeof(_psk));
  _keyLen = p.getBytes("key", _key, sizeof(_key));
  p.end();
}

static void _start() {
  if (!_ssid[0] || !_keyLen || !settings().wifi) return;
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  snprintf(_host, sizeof(_host), "claude-%02x%02x", mac[4], mac[5]);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(_host);
  WiFi.setSleep(true);                 // modem sleep: coexists with BLE, saves battery
  WiFi.begin(_ssid, _psk);
  _started = true; _lastTry = millis();
  Serial.printf("[wifi] connecting to '%s' as %s\n", _ssid, _host);
}

inline void wifiInit() { _load(); _start(); }

static void _stop() {
  if (_cli) _cli.stop();
  if (_srvUp) { _srv.end(); MDNS.end(); _srvUp = false; }
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  _started = false; _authed = false;
}

static void _sendHello() {
  uint8_t r[16]; esp_fill_random(r, sizeof(r));
  for (int i = 0; i < 16; i++) sprintf(_nonce + 2 * i, "%02x", r[i]);
  char b[64]; int n = snprintf(b, sizeof(b), "{\"hello\":1,\"nonce\":\"%s\"}\n", _nonce);
  _cli.write((const uint8_t*)b, n);
  _authDeadline = millis() + 5000;
}

static bool _checkAuth(const char* line) {
  JsonDocument d;
  if (deserializeJson(d, line)) return false;
  const char* cmd = d["cmd"]; const char* mac = d["mac"];
  if (!cmd || strcmp(cmd, "auth") || !mac) return false;
  uint8_t want[32], got[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), _key, _keyLen,
                  (const uint8_t*)_nonce, strlen(_nonce), want);
  if (_hex2bin(mac, got, sizeof(got)) != 32) return false;
  uint8_t diff = 0;                    // constant-time compare
  for (int i = 0; i < 32; i++) diff |= want[i] ^ got[i];
  return diff == 0;
}

inline void wifiTick() {
  if (!_started) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (_srvUp) { if (_cli) _cli.stop(); _srv.end(); MDNS.end(); _srvUp = false; _authed = false; }
    if (millis() - _lastTry > 15000) { _lastTry = millis(); WiFi.reconnect(); }
    return;
  }
  if (!_srvUp) {
    _srv.begin(); _srv.setNoDelay(true);
    if (MDNS.begin(_host)) MDNS.addService("claudebuddy", "tcp", PORT);
    _srvUp = true;
    Serial.printf("[wifi] up %s ip %s\n", _host, WiFi.localIP().toString().c_str());
  }
  WiFiClient nc = _srv.available();
  if (nc) {                            // newest client wins
    if (_cli) _cli.stop();
    _cli = nc; _cli.setNoDelay(true);
    _authed = false; _lineLen = 0;
    _sendHello();
  }
  if (_cli && !_cli.connected()) { _cli.stop(); _authed = false; }
  if (_cli && !_authed && (int32_t)(millis() - _authDeadline) > 0) { _cli.stop(); }
}

inline bool wifiPollLine(char* out, size_t cap) {
  while (_cli && _cli.available()) {
    char c = _cli.read();
    if (c == '\n' || c == '\r') {
      if (!_lineLen) continue;
      _line[_lineLen] = 0; _lineLen = 0;
      if (!_authed) {
        _authed = _checkAuth(_line);
        const char* r = _authed ? "{\"ack\":\"auth\",\"ok\":true,\"n\":0}\n"
                                : "{\"ack\":\"auth\",\"ok\":false,\"n\":0}\n";
        _cli.write((const uint8_t*)r, strlen(r));
        if (!_authed) _cli.stop();
        continue;
      }
      strncpy(out, _line, cap - 1); out[cap - 1] = 0;
      return true;
    }
    if (_lineLen < sizeof(_line) - 1) _line[_lineLen++] = c;
  }
  return false;
}

inline size_t wifiWrite(const uint8_t* data, size_t len) {
  if (!_cli || !_authed) return 0;
  return _cli.write(data, len);
}

inline bool wifiClientActive() { return _cli && _authed && _cli.connected(); }

inline const char* wifiStateName() {
  if (!_started) return _ssid[0] ? "off" : "unset";
  if (WiFi.status() != WL_CONNECTED) return "joining";
  return wifiClientActive() ? "linked" : "up";
}

inline bool wifiCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;
  if (!strcmp(cmd, "wifi_set")) {
    const char* ssid = doc["ssid"]; const char* psk = doc["psk"]; const char* key = doc["key"];
    uint8_t k[32]; int kl = key ? _hex2bin(key, k, sizeof(k)) : -1;
    bool ok = ssid && *ssid && strlen(ssid) < sizeof(_ssid) && psk && strlen(psk) < sizeof(_psk) && kl >= 16;
    if (ok) {
      Preferences p; p.begin("wifi", false);
      p.putString("ssid", ssid); p.putString("psk", psk); p.putBytes("key", k, kl);
      p.end();
      settings().wifi = true; settingsSave();
      _stop(); _load(); _start();
    }
    _wifiAck("wifi_set", ok);
    return true;
  }
  if (!strcmp(cmd, "wifi_status")) {
    char extra[96];
    // code: WiFi.status() 1=NO_SSID_AVAIL 4=CONNECT_FAILED 6=DISCONNECTED 3=CONNECTED
    snprintf(extra, sizeof(extra), ",\"state\":\"%s\",\"code\":%d,\"host\":\"%s\",\"ip\":\"%s\",\"rssi\":%d",
             wifiStateName(), (int)WiFi.status(), _host,
             WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "",
             WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0);
    _wifiAck("wifi_status", true, extra);
    return true;
  }
  if (!strcmp(cmd, "wifi_scan")) {           // 2.4 GHz networks the stick can see
    // Pause join attempts: a STA that keeps (re)connecting makes scans fail.
    bool was = _started;
    if (was) { WiFi.disconnect(false); }
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, true, false, 300);
    if (was) { WiFi.begin(_ssid, _psk); _lastTry = millis(); }
    String out = "{\"ack\":\"wifi_scan\",\"ok\":true,\"n\":" + String(n) + ",\"nets\":[";
    for (int i = 0; i < n && i < 20; i++) {
      String ss = WiFi.SSID(i); ss.replace("\"", "'");
      out += String(i ? "," : "") + "[\"" + ss + "\"," + WiFi.RSSI(i) + "," + WiFi.channel(i) + "," + (int)WiFi.encryptionType(i) + "]";
    }
    out += "]}\n";
    WiFi.scanDelete();
    Serial.print(out); bleWrite((const uint8_t*)out.c_str(), out.length()); wifiWrite((const uint8_t*)out.c_str(), out.length());
    return true;
  }
  if (!strcmp(cmd, "wifi_off")) {
    settings().wifi = false; settingsSave(); _stop();
    _wifiAck("wifi_off", true);
    return true;
  }
  return false;
}
