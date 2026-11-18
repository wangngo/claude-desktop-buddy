#pragma once
// Daikin AC over IR (send only). AC remotes send the WHOLE state each press,
// so the stick keeps the state (NVS "ac") and re-sends it on every change.
// Exact remote model unknown → the protocol variant is selectable; the user
// tries each one with "test" and keeps the one the AC beeps to.
// StickS3: IR TX = G46, powered from EXT_5V (off by default in M5Unified);
// EXT_5V: only ever switched ON (never toggled off — that reset the stick).
#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#ifdef BUDDY_IR_TX
#include <IRremoteESP8266.h>
#include <IRac.h>
#include <M5Unified.h>

struct AcProto { decode_type_t t; const char* name; };
static const AcProto AC_PROTOS[] = {
  {DAIKIN, "DAIKIN"},       {DAIKIN2, "DAIKIN2"},     {DAIKIN216, "DAIKIN216"},
  {DAIKIN160, "DAIKIN160"}, {DAIKIN176, "DAIKIN176"}, {DAIKIN128, "DAIKIN128"},
  {DAIKIN152, "DAIKIN152"}, {DAIKIN64, "DAIKIN64"},   {DAIKIN200, "DAIKIN200"},
  {DAIKIN312, "DAIKIN312"},
};
static const uint8_t AC_PROTO_N = sizeof(AC_PROTOS) / sizeof(AC_PROTOS[0]);
static const char* const AC_MODES[] = {"auto", "cool", "dry", "fan", "heat"};
static const char* const AC_FANS[]  = {"auto", "low", "med", "high"};

struct AcState { bool power = false; uint8_t temp = 26, mode = 1, fan = 0, proto = 0; };
static AcState _ac;
static IRac _irac(BUDDY_IR_TX);
static uint32_t _acSentAt = 0;
static bool _acLastOk = false;

inline void acLoad() {
  Preferences p; p.begin("ac", true);
  _ac.power = p.getBool("pw", false);
  _ac.temp  = constrain(p.getUChar("t", 26), 16, 32);
  _ac.mode  = p.getUChar("m", 1) % 5;
  _ac.fan   = p.getUChar("f", 0) % 4;
  _ac.proto = p.getUChar("p", 0) % AC_PROTO_N;
  p.end();
}

static void _acSave() {
  Preferences p; p.begin("ac", false);
  p.putBool("pw", _ac.power); p.putUChar("t", _ac.temp); p.putUChar("m", _ac.mode);
  p.putUChar("f", _ac.fan);   p.putUChar("p", _ac.proto);
  p.end();
}

static stdAc::opmode_t _acMode(uint8_t m) {
  switch (m) {
    case 1: return stdAc::opmode_t::kCool;
    case 2: return stdAc::opmode_t::kDry;
    case 3: return stdAc::opmode_t::kFan;
    case 4: return stdAc::opmode_t::kHeat;
    default: return stdAc::opmode_t::kAuto;
  }
}
static stdAc::fanspeed_t _acFan(uint8_t f) {
  switch (f) {
    case 1: return stdAc::fanspeed_t::kLow;
    case 2: return stdAc::fanspeed_t::kMedium;
    case 3: return stdAc::fanspeed_t::kHigh;
    default: return stdAc::fanspeed_t::kAuto;
  }
}

// Send the current state. EXT_5V is switched off again on every path.
inline bool acSend() {
  stdAc::state_t s;
  IRac::initState(&s);
  s.protocol = AC_PROTOS[_ac.proto].t;
  s.model    = -1;
  s.power    = _ac.power;
  s.mode     = _acMode(_ac.mode);
  s.celsius  = true;
  s.degrees  = _ac.temp;
  s.fanspeed = _acFan(_ac.fan);
  // M5Unified leaves the EXT_5V boost ON on StickS3 (measured: PM1 boost=1,
  // 5VOUT 5.16 V at idle). Toggling it reset the stick (send 1 OK with
  // off-after, send 2 re-enabled it → reset), so only ever switch it on.
  if (!M5.Power.getExtOutput()) { M5.Power.setExtOutput(true); delay(200); }
  bool ok = IRac::isProtocolSupported(s.protocol) && _irac.sendAc(s, nullptr);
  _acSentAt = millis(); _acLastOk = ok;
  Serial.printf("[ac] %s %s %uC %s fan %s -> %s\n", AC_PROTOS[_ac.proto].name,
                _ac.power ? "on" : "off", _ac.temp, AC_MODES[_ac.mode], AC_FANS[_ac.fan],
                ok ? "sent" : "FAILED");
  return ok;
}

static int _idxOf(const char* v, const char* const* list, int n) {
  for (int i = 0; i < n; i++) if (v && !strcmp(v, list[i])) return i;
  return -1;
}

// Apply fields from JSON {power, temp, delta, mode, fan, proto, send}; then send.
inline bool acApply(JsonDocument& d) {
  if (!d["power"].isNull()) _ac.power = d["power"].as<bool>();
  if (!d["temp"].isNull())  _ac.temp  = constrain(d["temp"].as<int>(), 16, 32);
  if (!d["delta"].isNull()) _ac.temp  = constrain((int)_ac.temp + d["delta"].as<int>(), 16, 32);
  int i;
  if ((i = _idxOf(d["mode"], AC_MODES, 5)) >= 0) _ac.mode = i;
  if ((i = _idxOf(d["fan"], AC_FANS, 4)) >= 0)   _ac.fan = i;
  if (!d["proto"].isNull()) {
    const char* pn = d["proto"];
    for (uint8_t k = 0; k < AC_PROTO_N; k++) if (pn && !strcmp(pn, AC_PROTOS[k].name)) _ac.proto = k;
  }
  _acSave();
  return acSend();
}

inline void acToJson(JsonObject o) {
  o["power"] = _ac.power; o["temp"] = _ac.temp;
  o["mode"] = AC_MODES[_ac.mode]; o["fan"] = AC_FANS[_ac.fan];
  o["proto"] = AC_PROTOS[_ac.proto].name;
  o["lastOk"] = _acLastOk; o["sentAgo"] = _acSentAt ? (millis() - _acSentAt) / 1000 : -1;
  JsonArray a = o["protos"].to<JsonArray>();
  for (uint8_t k = 0; k < AC_PROTO_N; k++)
    if (IRac::isProtocolSupported(AC_PROTOS[k].t)) a.add(AC_PROTOS[k].name);
  JsonArray m = o["modes"].to<JsonArray>(); for (auto x : AC_MODES) m.add(x);
  JsonArray f = o["fans"].to<JsonArray>();  for (auto x : AC_FANS) f.add(x);
}
inline bool acAvailable() { return true; }

#else   // no IR transmitter on this board/env
inline void acLoad() {}
inline bool acApply(JsonDocument&) { return false; }
inline void acToJson(JsonObject o) { o["error"] = "no IR on this build"; }
inline bool acAvailable() { return false; }
#endif
