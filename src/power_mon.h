#pragma once
// Power monitor: samples battery/charge/die-temp every 5 s and (by default)
// emits {"evt":"pwr",...} every 30 s so the bridge can log heat/drain A/B
// tests. M5PM1 (StickS3) measures voltages only — no battery/charge current
// (datasheet v1.9 ADC: VBAT, VREF, GPIO1/2, chip temp) — so the log carries
// voltage, voltage-derived %, charge flag and ESP32 die temperature.
#include "hal.h"
#include "ble_bridge.h"
#include "wifi_link.h"
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp32s3/rom/rtc.h>

// Why the chip last reset (esp_reset_reason), for crash/brownout triage.
inline const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_SW:       return "sw";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP:return "deepsleep";
    default: { static char b[12]; snprintf(b, sizeof(b), "r%d", (int)esp_reset_reason()); return b; }
  }
}

struct PwrSample {
  int16_t mV  = 0;      // battery voltage, mV (<=0: read error)
  int8_t  pct = -1;     // M5.Power.getBatteryLevel() (voltage curve), -1 = unknown
  bool    chg = false;  // PMIC reports charging
  float   tC  = 0;      // ESP32 die temperature, °C
};

static PwrSample _pwr;
static uint32_t  _pwrSampleAt = 0, _pwrLogAt = 0;
static uint32_t  _pwrLogMs = 30000;      // 0 = logging off

static void _pwrSample() {
  _pwr.mV  = M5.Power.getBatteryVoltage();
  int32_t l = M5.Power.getBatteryLevel();
  _pwr.pct = (l < 0 || l > 100) ? -1 : (int8_t)l;
  _pwr.chg = M5.Power.isCharging() == m5::Power_Class::is_charging;
  _pwr.tC  = temperatureRead();
}

inline const PwrSample& power() { return _pwr; }

// History for the web chart: one point per 30 s, 240 points = 2 h, RAM only.
struct PwrPoint { uint32_t up; int16_t mV; int8_t pct; uint8_t chg; int16_t tC10; };
static const uint16_t PWR_HIST_N = 240;
static PwrPoint _pwrHist[PWR_HIST_N];
static uint16_t _pwrHistHead = 0, _pwrHistLen = 0;
static uint32_t _pwrHistAt = 0;

static void _pwrHistPush() {
  PwrPoint& q = _pwrHist[_pwrHistHead];
  q.up = millis() / 1000; q.mV = _pwr.mV; q.pct = _pwr.pct; q.chg = _pwr.chg;
  q.tC10 = (int16_t)(_pwr.tC * 10);
  _pwrHistHead = (_pwrHistHead + 1) % PWR_HIST_N;
  if (_pwrHistLen < PWR_HIST_N) _pwrHistLen++;
}
inline uint16_t powerHistLen() { return _pwrHistLen; }
// i = 0 oldest .. len-1 newest
inline const PwrPoint& powerHistAt(uint16_t i) {
  return _pwrHist[(_pwrHistHead + PWR_HIST_N - _pwrHistLen + i) % PWR_HIST_N];
}

static void _pwrEmit(const char* key) {   // key: "evt":"pwr" or "ack":"power_log"
  char b[230];
  int n = snprintf(b, sizeof(b),
    "{%s,\"ok\":true,\"n\":0,\"up\":%lu,\"mV\":%d,\"pct\":%d,\"chg\":%s,\"tC\":%.1f,"
    "\"wifi\":\"%s\",\"bri\":%u,\"logSec\":%lu,\"rst\":\"%s\"}\n",
    key, millis() / 1000, _pwr.mV, _pwr.pct, _pwr.chg ? "true" : "false", _pwr.tC,
    wifiStateName(), M5.Display.getBrightness(), _pwrLogMs / 1000, resetReasonName());
  Serial.write(b, n); bleWrite((const uint8_t*)b, n); wifiWrite((const uint8_t*)b, n);
}

inline void powerTick() {
  uint32_t now = millis();
  if (!_pwrSampleAt || now - _pwrSampleAt >= 5000) { _pwrSampleAt = now; _pwrSample(); }
  if (!_pwrHistAt || now - _pwrHistAt >= 30000) { _pwrHistAt = now; _pwrHistPush(); }
  if (_pwrLogMs && now - _pwrLogAt >= _pwrLogMs) { _pwrLogAt = now; _pwrEmit("\"evt\":\"pwr\""); }
}

// {"cmd":"pm1"}: read-only PMIC/reset diagnostics (never toggles the boost).
// rawRst = ROM reset code (0x15 USB-UART, 0x16 USB-JTAG, 0x17 power glitch,
// 0x0f brownout, 0x01 power-on); src bits: 1 VIN(USB), 2 5VINOUT, 4 battery.
inline bool pm1Command(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd || strcmp(cmd, "pm1") != 0) return false;
  auto& pm = M5.Power.M5pm1;
  char b[200];
  int n = snprintf(b, sizeof(b),
    "{\"ack\":\"pm1\",\"ok\":true,\"n\":0,\"rawRst\":\"0x%02x\",\"wake\":\"0x%02x\",\"src\":%u,"
    "\"v5out\":%u,\"boost\":%s,\"vbat\":%d}\n",
    (unsigned)rtc_get_reset_reason(0), pm.getWakeSource(), (unsigned)pm.getPowerSource(),
    pm.get5VoutVoltage(), pm.getExtOutput() ? "true" : "false", M5.Power.getBatteryVoltage());
  Serial.write(b, n); bleWrite((const uint8_t*)b, n); wifiWrite((const uint8_t*)b, n);
  return true;
}

// {"cmd":"power_log","sec":N}: N=0 off, 5..3600 period; no "sec" = read now.
inline bool powerCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd || strcmp(cmd, "power_log") != 0) return false;
  if (!doc["sec"].isNull()) {
    long s = doc["sec"].as<long>();
    _pwrLogMs = s <= 0 ? 0 : (uint32_t)constrain(s, 5L, 3600L) * 1000;
  }
  _pwrSample();
  _pwrEmit("\"ack\":\"power_log\"");
  return true;
}
