#pragma once
// Diagnostic raw IR capture on the StickS3 receiver (G42, powered from
// EXT_5V). The boost couples ~8.7 kHz noise onto the receiver output, so the
// stick records EVERY edge (time + level) into PSRAM and dumps it; filtering
// and decoding happen on the host (tools/ir_capture.py) so thresholds can be
// tuned without reflashing.
// {"cmd":"ir_capture","ms":6000} → blocks for ms, then streams
//   {"ack":"ir_capture","ok":true,"n":0,"edges":N,"ms":..,"lost":L}
//   {"ircap":"<base64 chunk of uint32 LE: (micros & ~1) | level>"} ...
//   {"ircap_end":1}
#include <Arduino.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <M5Unified.h>

#ifdef BUDDY_IR_TX
static const int IR_RX_PIN = 42;
static const uint32_t IRCAP_MAX = 200000;          // edges (800 KB PSRAM)
static uint32_t* _ircap = nullptr;
static volatile uint32_t _ircapN = 0, _ircapLost = 0;

static void IRAM_ATTR _ircapIsr() {
  uint32_t t = micros();
  uint32_t lvl = (uint32_t)gpio_get_level((gpio_num_t)IR_RX_PIN);
  if (_ircapN < IRCAP_MAX) _ircap[_ircapN++] = (t & ~1u) | lvl;
  else _ircapLost++;
}

inline bool irCaptureCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd || strcmp(cmd, "ir_capture") != 0) return false;
  uint32_t ms = constrain(doc["ms"] | 6000, 500, 15000);
  if (!_ircap) _ircap = (uint32_t*)ps_malloc(IRCAP_MAX * sizeof(uint32_t));
  if (!_ircap) { Serial.print("{\"ack\":\"ir_capture\",\"ok\":false,\"n\":0,\"error\":\"no psram\"}\n"); return true; }
  // {"boost":false}: switch the EXT_5V boost OFF for a clean capture (works
  // only if USB VBUS feeds the 5 V rail — v5out tells). Re-enabling the boost
  // at runtime reset the stick, so pair it with {"reboot":true}.
  bool boost = doc["boost"] | true;
  if (boost && !M5.Power.getExtOutput()) { M5.Power.setExtOutput(true); delay(200); }
  if (!boost) { M5.Power.setExtOutput(false); delay(300); }
  uint16_t v5 = M5.Power.M5pm1.get5VoutVoltage();
  pinMode(IR_RX_PIN, INPUT);
  _ircapN = 0; _ircapLost = 0;
  uint32_t t0 = micros();
  _ircap[_ircapN++] = (t0 & ~1u) | (uint32_t)digitalRead(IR_RX_PIN);   // initial level
  attachInterrupt(IR_RX_PIN, _ircapIsr, CHANGE);
  delay(ms);
  detachInterrupt(IR_RX_PIN);
  uint32_t n = _ircapN;
  Serial.printf("{\"ack\":\"ir_capture\",\"ok\":true,\"n\":0,\"edges\":%lu,\"ms\":%lu,\"lost\":%lu,\"boost\":%s,\"v5out\":%u}\n",
                (unsigned long)n, (unsigned long)ms, (unsigned long)_ircapLost,
                M5.Power.getExtOutput() ? "true" : "false", v5);
  // 750 edges (3000 B) per line → 4000 base64 chars.
  static unsigned char b64[4100];
  for (uint32_t i = 0; i < n; i += 750) {
    uint32_t cnt = min<uint32_t>(750, n - i);
    size_t olen = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char*)&_ircap[i], cnt * 4);
    Serial.print("{\"ircap\":\""); Serial.write(b64, olen); Serial.print("\"}\n");
  }
  Serial.print("{\"ircap_end\":1}\n");
  Serial.flush();
  if (doc["reboot"] | false) { delay(200); ESP.restart(); }
  return true;
}
#else
inline bool irCaptureCommand(JsonDocument&) { return false; }
#endif
