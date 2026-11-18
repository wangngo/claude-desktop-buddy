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
// Join state machine. The core's auto-reconnect is OFF: with it on, every
// NO_AP_FOUND immediately re-runs begin() (WiFiGeneric.cpp:1084) → endless
// all-channel scanning → the stick runs hot. We own every retry instead and
// switch the radio fully off between attempts.
static bool     _trying = false, _gaveUp = false;
static uint8_t  _fails = 0;
static int      _lastCode = 0;
static volatile uint8_t _discReason = 0;   // last STA disconnect reason (wifi_err_reason_t)
static bool     _evtHooked = false;
static const uint32_t BACKOFF_MS[] = {15000, 15000, 60000, 60000, 300000};
static const uint8_t  MAX_FAILS = 10;
// Full 20 dBm TX bursts (active scan) reset the StickS3 (3/3 runs, reset
// reason UNKNOWN, PMIC charge flag drops at the same moment); passive scan
// and 8.5 dBm active scan did not (2/2). Cap TX power for every radio use.
static const wifi_power_t TX_CAP = WIFI_POWER_15dBm;
// SoftAP setup mode (from the device menu): 5 min, WPA2, pass on screen.
static bool     _ap = false;
static uint32_t _apUntil = 0;
static char     _apPass[9] = "";
static char     _ip[16] = "";
// web_ui.h (included after this file) provides these.
static void webBegin(); static void webEnd(); static void webTick();

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

static void _mkHost() {
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  snprintf(_host, sizeof(_host), "claude-%02x%02x", mac[4], mac[5]);
}

static void _attempt() {
  if (!_evtHooked) {
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
      _discReason = info.wifi_sta_disconnected.reason;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    _evtHooked = true;
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(TX_CAP);
  WiFi.setAutoReconnect(false);
  WiFi.setHostname(_host);
  WiFi.setSleep(true);                 // modem sleep: coexists with BLE, saves battery
  WiFi.begin(_ssid, _psk);
  _trying = true; _lastTry = millis();
  Serial.printf("[wifi] join '%s' as %s (try %u)\n", _ssid, _host, _fails + 1);
}

static void _start() {
  _mkHost();
  if (!_ssid[0] || !settings().wifi) return;
  _started = true; _gaveUp = false; _fails = 0;
  _attempt();
}

inline void wifiInit() { _load(); _start(); }

static void _stop() {
  if (_cli) _cli.stop();
  if (_srvUp) { _srv.end(); MDNS.end(); _srvUp = false; }
  webEnd();
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  _started = false; _authed = false; _trying = false; _ip[0] = 0;
}

// Store creds (from USB wifi_set, web or SoftAP) and (re)join. Keeps the
// existing HMAC key; creates one if none so the TCP transport can start.
static bool _wifiStore(const char* ssid, const char* psk, const uint8_t* key, int keyLen) {
  if (!ssid || !*ssid || strlen(ssid) >= sizeof(_ssid) || !psk || strlen(psk) >= sizeof(_psk)) return false;
  Preferences p; p.begin("wifi", false);
  p.putString("ssid", ssid); p.putString("psk", psk);
  if (key && keyLen >= 16) p.putBytes("key", key, keyLen);
  else if (!p.getBytesLength("key")) { uint8_t k[32]; esp_fill_random(k, 32); p.putBytes("key", k, 32); }
  p.end();
  settings().wifi = true; settingsSave();
  return true;
}

inline bool wifiApActive() { return _ap; }
inline const char* wifiApSsid() { return _host; }
inline const char* wifiApPass() { return _apPass; }
inline uint32_t wifiApLeftSec() { return _ap ? (uint32_t)max<int32_t>(0, (int32_t)(_apUntil - millis()) / 1000) : 0; }
inline const char* wifiIpStr() { return _ip; }

inline void wifiApStart() {
  if (_ap) { _apUntil = millis() + 300000; return; }
  _stop();
  _mkHost();
  for (int i = 0; i < 8; i++) _apPass[i] = '0' + esp_random() % 10;
  _apPass[8] = 0;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);              // STA half lets the page scan for networks
  WiFi.setAutoReconnect(false);
  WiFi.softAP(_host, _apPass);
  WiFi.setTxPower(TX_CAP);
  snprintf(_ip, sizeof(_ip), "%s", WiFi.softAPIP().toString().c_str());
  _ap = true; _apUntil = millis() + 300000;
  webBegin();
  Serial.printf("[wifi] setup AP '%s' ip %s\n", _host, _ip);
}

// Leave setup mode in `ms` (lets the HTTP reply go out first), then join.
inline void wifiApStopSoon(uint32_t ms) { if (_ap) _apUntil = millis() + ms; }

static void _apStop() {
  webEnd();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  _ap = false; _ip[0] = 0;
  _load(); _start();
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
  if (_ap) {
    webTick();
    if ((int32_t)(millis() - _apUntil) > 0) _apStop();
    return;
  }
  if (!_started) return;
  wl_status_t st = WiFi.status();
  if (st != WL_CONNECTED) {
    if (_srvUp) { if (_cli) _cli.stop(); _srv.end(); webEnd(); MDNS.end(); _srvUp = false; _authed = false; _ip[0] = 0; }
    uint32_t now = millis();
    if (_trying) {
      if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED || now - _lastTry > 20000) {
        _lastCode = st; _trying = false; _fails++; _lastTry = now;
        WiFi.disconnect(true); WiFi.mode(WIFI_OFF);        // radio off until next try
        Serial.printf("[wifi] join failed code %d reason %u (%u/%u)\n", st, _discReason, _fails, MAX_FAILS);
        if (_fails >= MAX_FAILS) { _gaveUp = true; _started = false; Serial.println("[wifi] giving up until reboot / new creds"); }
      }
      return;
    }
    uint8_t bi = _fails < 5 ? _fails : 4;
    if (now - _lastTry > BACKOFF_MS[bi]) _attempt();
    return;
  }
  _trying = false; _fails = 0; _lastCode = WL_CONNECTED;
  if (!_srvUp) {
    if (_keyLen) { _srv.begin(); _srv.setNoDelay(true); }
    if (MDNS.begin(_host)) {
      if (_keyLen) MDNS.addService("claudebuddy", "tcp", PORT);
      MDNS.addService("http", "tcp", 80);
    }
    webBegin();
    _srvUp = true;
    snprintf(_ip, sizeof(_ip), "%s", WiFi.localIP().toString().c_str());
    Serial.printf("[wifi] up %s ip %s\n", _host, _ip);
  }
  webTick();
  if (!_keyLen) return;
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
  if (_ap) return "setup";
  if (_gaveUp) return "gaveup";
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
    bool ok = kl >= 16 && _wifiStore(ssid, psk, k, kl);
    if (ok) {
      if (_ap) { _ap = false; webEnd(); WiFi.softAPdisconnect(true); }
      _stop(); _load(); _start();
    }
    _wifiAck("wifi_set", ok);
    return true;
  }
  if (!strcmp(cmd, "wifi_status")) {
    char extra[128];
    // code: WiFi.status() 1=NO_SSID_AVAIL 4=CONNECT_FAILED 6=DISCONNECTED 3=CONNECTED
    snprintf(extra, sizeof(extra), ",\"state\":\"%s\",\"code\":%d,\"last\":%d,\"reason\":%u,\"fails\":%u,\"host\":\"%s\",\"ip\":\"%s\",\"rssi\":%d",
             wifiStateName(), (int)WiFi.status(), _lastCode, _discReason, _fails, _host,
             WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "",
             WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0);
    _wifiAck("wifi_status", true, extra);
    return true;
  }
  if (!strcmp(cmd, "wifi_scan")) {           // 2.4 GHz networks the stick can see
    // Pause join attempts: a STA that keeps (re)connecting makes scans fail.
    bool was = _started && WiFi.status() == WL_CONNECTED;
    bool radioWasOff = WiFi.getMode() == WIFI_OFF;
    if (_trying) { WiFi.disconnect(false); _trying = false; _lastTry = millis(); }
    if (radioWasOff) WiFi.mode(WIFI_STA);
    // Diagnostics: {"passive":true} = listen only (no TX), {"txp":N} = TX power, 0.25 dBm units.
    bool passive = doc["passive"] | false;
    WiFi.setTxPower(doc["txp"].isNull() ? TX_CAP : (wifi_power_t)doc["txp"].as<int>());
    Serial.printf("[wifi] scan passive=%d txp=%d\n", passive, (int)WiFi.getTxPower());
    Serial.flush();
    int n = WiFi.scanNetworks(false, true, passive, 300);
    String out = "{\"ack\":\"wifi_scan\",\"ok\":true,\"n\":" + String(n) + ",\"nets\":[";
    for (int i = 0; i < n && i < 20; i++) {
      String ss = WiFi.SSID(i); ss.replace("\"", "'");
      out += String(i ? "," : "") + "[\"" + ss + "\"," + WiFi.RSSI(i) + "," + WiFi.channel(i) + "," + (int)WiFi.encryptionType(i) + "]";
    }
    out += "]}\n";
    WiFi.scanDelete();
    if (radioWasOff && !_ap) WiFi.mode(WIFI_OFF);   // don't leave the radio on
    (void)was;
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
