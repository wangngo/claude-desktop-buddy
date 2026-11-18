#pragma once
// Web control page served by the stick on port 80 (STA when joined, or the
// SoftAP setup network). Included once from xfer.h, after wifi_impl.h.
//
// Security (home LAN, plain HTTP — the PIN travels unencrypted):
//  - HTTP Basic auth, user "buddy", 6-digit PIN (NVS "web"/"pin", shown on
//    the stick's DEVICE info page and on the setup card).
//  - 5 wrong PINs → 60 s lockout.
//  - Every POST needs header X-Buddy: 1 (forces a CORS preflight, so another
//    site can't drive the stick with the browser's cached credentials).
//  - Host header must be our IP / <host>.local (blocks DNS rebinding).
//  - The WiFi password is never returned.
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include "web_page.h"
#include "ac_ir.h"
#include "power_mon.h"

extern uint8_t brightLevel;            // main.cpp

static WebServer _web(80);
static bool     _webUp = false, _webRouted = false;
static char     _pin[7] = "";
static uint8_t  _webFails = 0;
static uint32_t _webLockUntil = 0, _webLastReq = 0, _webRebootAt = 0;
static bool     _scanRunning = false;
// WiFi changes requested over HTTP run after the reply is sent (stopping the
// server from inside its own handler is unsafe): 1 = rejoin, 2 = radio off.
static uint8_t  _webAction = 0;
static uint32_t _webActionAt = 0;

inline const char* webPin() {
  if (!_pin[0]) {
    Preferences p; p.begin("web", false);
    if (p.getString("pin", _pin, sizeof(_pin)) != 6) {
      for (int i = 0; i < 6; i++) _pin[i] = '0' + esp_random() % 10;
      _pin[6] = 0; p.putString("pin", _pin);
    }
    p.end();
  }
  return _pin;
}

inline bool webRecent() { return _webUp && _webLastReq && millis() - _webLastReq < 10000; }

static void _webJson(int code, JsonDocument& d) {
  String out; serializeJson(d, out);
  _web.sendHeader("Cache-Control", "no-store");
  _web.send(code, "application/json", out);
}
static void _webErr(int code, const char* msg) {
  JsonDocument d; d["ok"] = false; d["error"] = msg; _webJson(code, d);
}

static bool _hostOk() {
  String h = _web.hostHeader();
  int c = h.indexOf(':'); if (c >= 0) h = h.substring(0, c);
  h.toLowerCase();
  String local = String(_host) + ".local"; local.toLowerCase();
  return h == _ip || h == local || h == "192.168.4.1";
}

// Gate for every route. post=true also requires the X-Buddy header.
static bool _gate(bool post) {
  _webLastReq = millis();
  if (!_hostOk()) { _webErr(403, "bad host"); return false; }
  if ((int32_t)(_webLockUntil - millis()) > 0) { _webErr(429, "locked, wait 60s"); return false; }
  if (!_web.authenticate("buddy", webPin())) {
    if (_web.hasHeader("Authorization") && ++_webFails >= 5) {
      _webFails = 0; _webLockUntil = millis() + 60000;
      Serial.println("[web] 5 bad PINs -> 60 s lockout");
    }
    _web.requestAuthentication(BASIC_AUTH, "Claude Buddy (PIN on stick)");
    return false;
  }
  _webFails = 0;
  if (post && _web.header("X-Buddy") != "1") { _webErr(403, "missing X-Buddy header"); return false; }
  return true;
}

static bool _body(JsonDocument& d) {
  if (deserializeJson(d, _web.arg("plain"))) { _webErr(400, "bad json"); return false; }
  return true;
}

static void _apiState() {
  if (!_gate(false)) return;
  JsonDocument d;
  JsonObject dev = d["dev"].to<JsonObject>();
  Settings& s = settings();
  dev["bright"] = brightLevel; dev["vol"] = s.vol; dev["sound"] = s.sound;
  dev["offSec"] = s.offSec; dev["offUsb"] = s.offUsb;
  dev["up"] = millis() / 1000;
  dev["heapInt"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  dev["psram"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  JsonObject w = d["wifi"].to<JsonObject>();
  w["state"] = wifiStateName(); w["enabled"] = s.wifi; w["ssid"] = _ssid;
  w["host"] = _host; w["ip"] = _ip;
  w["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  w["ap"] = _ap; w["apLeft"] = wifiApLeftSec();
  const PwrSample& p = power();
  JsonObject pw = d["pwr"].to<JsonObject>();
  pw["mV"] = p.mV; pw["pct"] = p.pct; pw["chg"] = p.chg; pw["tC"] = serialized(String(p.tC, 1));
  acToJson(d["ac"].to<JsonObject>());
  _webJson(200, d);
}

static void _apiSet() {
  if (!_gate(true)) return;
  JsonDocument b; if (!_body(b)) return;
  Settings& s = settings();
  if (!b["bright"].isNull()) {
    brightLevel = constrain(b["bright"].as<int>(), 0, 4);
    s.bright = brightLevel; HAxp.ScreenBreath(20 + brightLevel * 20);
  }
  if (!b["vol"].isNull())    { s.vol = constrain(b["vol"].as<int>(), 0, 255); M5.Speaker.setVolume(s.vol); }
  if (!b["sound"].isNull())  s.sound = b["sound"].as<bool>();
  if (!b["offSec"].isNull()) s.offSec = constrain(b["offSec"].as<int>(), 0, 3600);
  if (!b["offUsb"].isNull()) s.offUsb = b["offUsb"].as<bool>();
  settingsSave();
  JsonDocument d; d["ok"] = true; _webJson(200, d);
}

static void _apiWifi() {
  if (!_gate(true)) return;
  JsonDocument b; if (!_body(b)) return;
  JsonDocument d;
  if (!b["ssid"].isNull()) {
    bool ok = _wifiStore(b["ssid"].as<const char*>(), b["psk"] | "", nullptr, 0);
    d["ok"] = ok;
    if (!ok) d["error"] = "ssid/psk too long or empty";
    _webJson(ok ? 200 : 400, d);
    if (ok) {
      if (_ap) wifiApStopSoon(1500);    // reply first, then leave setup mode and join
      else { _webAction = 1; _webActionAt = millis() + 1500; }
    }
    return;
  }
  if (!b["enabled"].isNull()) {
    bool en = b["enabled"].as<bool>();
    settings().wifi = en; settingsSave();
    d["ok"] = true; _webJson(200, d);
    if (!en) { _webAction = 2; _webActionAt = millis() + 1000; }
    else if (!_started) { _load(); _start(); }
    return;
  }
  _webErr(400, "need ssid or enabled");
}

static void _apiScanStart() {
  if (!_gate(true)) return;
  if (!_scanRunning) {
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    // passive: listen only, no TX bursts (see TX_CAP note in wifi_impl.h)
    _scanRunning = WiFi.scanNetworks(true, false, true, 300) == WIFI_SCAN_RUNNING;
  }
  JsonDocument d; d["ok"] = _scanRunning; _webJson(200, d);
}

static void _apiScanGet() {
  if (!_gate(false)) return;
  JsonDocument d;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { d["running"] = true; _webJson(200, d); return; }
  _scanRunning = false;
  JsonArray a = d["nets"].to<JsonArray>();
  for (int i = 0; i < n && i < 25; i++) {
    JsonArray e = a.add<JsonArray>();
    e.add(WiFi.SSID(i)); e.add(WiFi.RSSI(i)); e.add(WiFi.channel(i));
  }
  if (n >= 0) WiFi.scanDelete();
  _webJson(200, d);
}

static void _apiPower() {
  if (!_gate(false)) return;
  // [[up_s, mV, pct, chg, tC*10], ...] oldest first; built as text (small).
  String out; out.reserve(powerHistLen() * 28 + 4);
  out = "[";
  for (uint16_t i = 0; i < powerHistLen(); i++) {
    const PwrPoint& q = powerHistAt(i);
    char b[48];
    snprintf(b, sizeof(b), "%s[%lu,%d,%d,%u,%d]", i ? "," : "", (unsigned long)q.up, q.mV, q.pct, q.chg, q.tC10);
    out += b;
  }
  out += "]";
  _web.sendHeader("Cache-Control", "no-store");
  _web.send(200, "application/json", out);
}

static void _apiAc() {
  if (!_gate(true)) return;
  JsonDocument b; if (!_body(b)) return;
  if (!acAvailable()) { _webErr(501, "no IR on this build"); return; }
  bool ok = acApply(b);
  JsonDocument d; d["ok"] = ok; if (!ok) d["error"] = "IR send failed";
  acToJson(d["ac"].to<JsonObject>());
  _webJson(ok ? 200 : 500, d);
}

static void _apiReboot() {
  if (!_gate(true)) return;
  JsonDocument d; d["ok"] = true; _webJson(200, d);
  _webRebootAt = millis() + 500;
}

static void webBegin() {
  if (_webUp) return;
  webPin();
  if (!_webRouted) {
    _web.on("/", HTTP_GET, [] {
      if (!_gate(false)) return;
      _web.sendHeader("Cache-Control", "no-store");
      _web.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
    });
    _web.on("/api/state", HTTP_GET, _apiState);
    _web.on("/api/set", HTTP_POST, _apiSet);
    _web.on("/api/wifi", HTTP_POST, _apiWifi);
    _web.on("/api/scan", HTTP_POST, _apiScanStart);
    _web.on("/api/scan", HTTP_GET, _apiScanGet);
    _web.on("/api/power", HTTP_GET, _apiPower);
    _web.on("/api/ac", HTTP_POST, _apiAc);
    _web.on("/api/reboot", HTTP_POST, _apiReboot);
    _web.onNotFound([] { _web.send(404, "text/plain", "not found"); });
    const char* hdrs[] = {"X-Buddy"};
    _web.collectHeaders(hdrs, 1);
    _webRouted = true;
  }
  _web.begin();
  _webUp = true;
  Serial.printf("[web] http://%s/ (user buddy, PIN on stick)\n", _ip);
}

static void webEnd() {
  if (!_webUp) return;
  _web.stop();
  _webUp = false; _scanRunning = false;
}

static void webTick() {
  if (_webUp) _web.handleClient();
  if (_webAction && (int32_t)(millis() - _webActionAt) > 0) {
    uint8_t a = _webAction; _webAction = 0;
    _stop();
    if (a == 1) { _load(); _start(); }
  }
  if (_webRebootAt && (int32_t)(millis() - _webRebootAt) > 0) ESP.restart();
}
