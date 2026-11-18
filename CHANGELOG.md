# Changelog (fork wangngo/claude-desktop-buddy)

## 2026-11-18 (2) — speaker: voice + sfx
- firmware `src/audio.h`: `sfx` tone patterns and `audio_begin/audio/audio_end` PCM playback (PSRAM buffer); PSRAM enabled (qio_opi, 8MB detected).
- bridge: macOS `say -v Linh` speech for permission prompts / long turns / limit thresholds; windowed acked audio transfer; boot handshake replaces the fixed 4s sleep.
- IR investigation (deferred): GPIO42 receiver output toggles ~17k edges/s whenever EXT_5V is on (0 when off). That is boost noise on the receiver, so learning from the remote is blocked; TX unaffected. Probe code in `tools/irprobe/`.

## 2026-11-18 — branch `m5sticks3`
- Port to M5Unified (`src/hal.h` shim); new default env `m5sticks3` (ESP32-S3); `m5stickc-plus` still builds.
- Fix: M5Unified does not call `Serial.begin` unless `cfg.serial_baudrate` is set; without it USB JSON and logs were dead.
- Usage limits: heartbeat extension `lim` {h5,h5t,d7,d7t}, shown on the HUD line and Info page CLAUDE → LIMITS.
- `bridge/`: Claude Code CLI ↔ stick over USB (http hooks + daemon + statusline tee + LaunchAgent). Verified: button A → `allow` round-trip, hooks and rate_limits reach the daemon from a live session.
