#pragma once
// Speaker features driven by the bridge:
//   {"cmd":"sfx","name":"done|error|warn|prompt"}         -> short tone pattern
//   {"cmd":"audio_begin","rate":16000,"len":<bytes>}      -> allocate buffer
//   {"cmd":"audio","d":"<base64 int16 LE mono>"}          -> append (acked with n=bytes so far)
//   {"cmd":"audio_end"}                                   -> play
// Everything is muted when settings().sound is off. Volume stays below the
// vendor's 75% guidance for battery operation.
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include "hal.h"
#include "stats.h"

static const uint8_t  AUDIO_VOLUME   = 150;            // of 255 (~59%)
static const uint32_t AUDIO_MAX_LEN  = 16000 * 2 * 8;  // 8 s @16 kHz int16
static const int      CH_CLIP = 0, CH_SFX = 7;          // sfx never cuts speech

static int16_t* _aBuf = nullptr;
static uint32_t _aLen = 0, _aCap = 0, _aRate = 16000;

struct _Tone { uint16_t f, ms, gap; };
static const _Tone SFX_DONE[]  = {{1318, 70, 30}, {1760, 110, 0}};
static const _Tone SFX_ERROR[] = {{440, 120, 60}, {330, 220, 0}};
static const _Tone SFX_WARN[]  = {{2093, 60, 70}, {2093, 60, 70}, {2093, 60, 0}};
static const _Tone SFX_PROMPT[]= {{1200, 80, 0}};
static const _Tone* _sfx = nullptr; static uint8_t _sfxN = 0, _sfxI = 0;
static uint32_t _sfxNext = 0;

// One fixed buffer for the whole uptime: never freed, so the speaker task
// can't read a dangling pointer if a new clip arrives mid-playback.
inline void audioInit() {
  M5.Speaker.setVolume(AUDIO_VOLUME);
  _aBuf = (int16_t*)ps_malloc(AUDIO_MAX_LEN);
  _aCap = _aBuf ? AUDIO_MAX_LEN : 0;
}

inline void sfxPlay(const char* name) {
  if (!settings().sound || !name) return;
  if      (!strcmp(name, "done"))   { _sfx = SFX_DONE;   _sfxN = 2; }
  else if (!strcmp(name, "error"))  { _sfx = SFX_ERROR;  _sfxN = 2; }
  else if (!strcmp(name, "warn"))   { _sfx = SFX_WARN;   _sfxN = 3; }
  else if (!strcmp(name, "prompt")) { _sfx = SFX_PROMPT; _sfxN = 1; }
  else return;
  _sfxI = 0; _sfxNext = 0;
}

// Call every loop: steps the tone sequencer and frees finished clips.
inline void audioTick() {
  uint32_t now = millis();
  if (_sfx && (int32_t)(now - _sfxNext) >= 0) {
    if (_sfxI < _sfxN) {
      const _Tone& t = _sfx[_sfxI++];
      M5.Speaker.tone(t.f, t.ms, CH_SFX, true);
      _sfxNext = now + t.ms + t.gap;
    } else {
      _sfx = nullptr;
    }
  }
}

inline bool audioCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (!strcmp(cmd, "sfx")) {
    sfxPlay(doc["name"]);
    _xAck("sfx", true, 0);
    return true;
  }
  if (!strcmp(cmd, "audio_begin")) {
    uint32_t len = doc["len"] | 0;
    _aRate = doc["rate"] | 16000;
    M5.Speaker.stop(CH_CLIP);
    for (int i = 0; i < 50 && M5.Speaker.isPlaying(CH_CLIP); i++) delay(2);
    _aLen = 0;
    bool ok = _aBuf && len > 0 && len <= _aCap && settings().sound;
    _xAck("audio_begin", ok, 0);
    return true;
  }
  if (!strcmp(cmd, "audio")) {
    const char* d = doc["d"];
    bool ok = false;
    if (_aBuf && d) {
      size_t out = 0;
      int rc = mbedtls_base64_decode((uint8_t*)_aBuf + _aLen, _aCap - _aLen, &out,
                                     (const uint8_t*)d, strlen(d));
      ok = rc == 0;
      if (ok) _aLen += out;
    }
    _xAck("audio", ok, _aLen);
    return true;
  }
  if (!strcmp(cmd, "audio_end")) {
    bool ok = _aBuf && _aLen >= 2;
    if (ok) {
      M5.Speaker.playRaw(_aBuf, _aLen / 2, _aRate, false, 1, CH_CLIP, true);
    }
    _xAck("audio_end", ok, _aLen);
    return true;   // buffer stays alive until the next audio_begin
  }
  return false;
}
