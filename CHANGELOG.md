# Changelog (fork wangngo/claude-desktop-buddy)

## 2026-11-21 — 5h limit no longer flips between sessions
- Bug: with 2 sessions on the same account, the stick's 5h % flipped (seen 1% vs 3%). `on_statusline` showed the most recently active session's own statusline snapshot, and snapshots differ in age.
- Fix: `pick_rate()` in `bridge/buddy_bridge.py`. It groups sessions by account (key = `seven_day.resets_at`) and follows the account of the most recently active session, as before. Per window it takes the newest `resets_at`, then the highest `used_percentage`.
- Tests: `bridge/test_buddy_bridge.py` (run `bridge/.venv/bin/python -m unittest discover -s bridge -v`).
- BLE link itself was healthy (`stick ready over BLE (Claude-A981)` 11:57).

## 2026-11-18 (5) — power monitor, WiFi heat fix, on-stick web UI, Daikin IR
- `src/power_mon.h`: 5 s samples (VBAT, voltage-%, PM1 charge flag, ESP32 die temp), `{"evt":"pwr"}` every 30 s → bridge `~/.config/claude-buddy/power.csv`; 2 h RAM history; reset reason; `power_log`, `pm1` (read-only PMIC diag) commands. M5PM1 has NO current sense (datasheet v1.9) — `GetBatCurrent()` is meaningless on S3; DEVICE page now uses power_mon.
- Battery icon on dashboard (landscape top-left, portrait top-right).
- WiFi heat root cause: core auto-reconnect re-runs begin() on every NO_AP_FOUND (WiFiGeneric.cpp:1084) → endless all-channel scanning. Fix: `setAutoReconnect(false)`, own backoff 15s→60s→5min, radio OFF between tries, give up after 10. Die temp 58 → 50 °C (short observation).
- Resets under load (reset reason 0 = USB/glitch, unmapped in IDF 4.4): WiFi active scan at 20 dBm 3/3 reset; 15 dBm / passive 0/7 → TX power capped at 15 dBm (STA, AP, scans); web scan passive.
- EXT_5V boost is ON from boot (M5Unified `cfg.output_power=true`, 5VOUT 5.16 V); re-enabling it at runtime reset the stick → IR send only ever turns it on. Boost off → 5VOUT 0.94 V (USB does NOT feed EXT_5V).
- `src/web_ui.h` + `web_page.h`: web page on port 80 (STA or SoftAP setup from menu → wifi, 5 min). Basic auth `buddy` + 6-digit PIN (NVS, shown on DEVICE page / setup card), 5-fail lockout, X-Buddy header on POST, Host check. Brightness, auto-off (+on USB), volume, sound, WiFi on/off/scan/set, reboot, AC panel, 2 h battery/temp chart. Built + flashed, NOT tested live (no 2.4 GHz network joinable).
- `src/ac_ir.h`: Daikin via IRremoteESP8266 2.9.0 IRac (send-only, 8 variants), state in NVS; serial/web `ac` command. 8 variants sent 21:57 — AC response not confirmed.
- `src/ir_capture.h` + `tools/ir_capture.py`: raw edge capture on G42 → host analysis. Boost noise ~35k edges/s; Daikin marks (~430 us) visible with remote pressed, but 1-bit spaces corrupted → no decode. IR paused by W.
- New settings in NVS: bright, vol, offSec, offUsb. Diag serial cmds: `spk`, `chg`, `bri`.
- Audible whine only on USB (W); charging-off listening test done (result pending from W). USB drops/resets on power-state changes while on USB (charge re-enable, boost off).

## 2026-11-18 (4) — big-type UI, Clawd, IMU gestures
- Landscape dashboard (240x135) and approval screen with large fonts and coloured 5h/7d bars; Clawd mascot pixel-drawn from the Claude Code logo, moods: sleep / idle bob / busy hop / wait wiggle+! / celebrate / dizzy.
- IMU (BMI270, verified on device: flat az≈+1.01, upright |ay|≈0.8–0.95, sideways |ax|≈0.99): upright → portrait list of recent commands, sideways → dashboard (auto flips 180°), face-down → do-not-disturb (screen off, muted, stick sends {"evt":"dnd"} so the bridge leaves prompts to the terminal), shake → dizzy Clawd.
- `imu` debug command + bridge `/imu`; rate limits per session (two accounts).

## 2026-11-18 (3) — BLE transport
- bridge: `BleLink` (bleak, NUS, bonded passkey pairing). Fallback when there is no USB, switches back when the cable returns. Write-without-response + line acks; speech at 8 kHz over BLE: 3.3 s clip in 5.7 s (was 37 s with per-packet responses). Verified: pairing, reconnect via bond after a daemon restart, speech, permission allow by button over BLE.
- firmware: BLE RX ring 2 KB → 8 KB (flash pending, needs USB).

## 2026-11-18 (2) — speaker: voice + sfx
- firmware `src/audio.h`: `sfx` tone patterns and `audio_begin/audio/audio_end` PCM playback (PSRAM buffer); PSRAM enabled (qio_opi, 8MB detected).
- bridge: macOS `say -v Linh` speech for permission prompts / long turns / limit thresholds; windowed acked audio transfer; boot handshake replaces the fixed 4s sleep.
- IR investigation (deferred): GPIO42 receiver output toggles ~17k edges/s whenever EXT_5V is on (0 when off). That is boost noise on the receiver, so learning from the remote is blocked; TX unaffected. Probe code in `tools/irprobe/`.

## 2026-11-18 — branch `m5sticks3`
- Port to M5Unified (`src/hal.h` shim); new default env `m5sticks3` (ESP32-S3); `m5stickc-plus` still builds.
- Fix: M5Unified does not call `Serial.begin` unless `cfg.serial_baudrate` is set; without it USB JSON and logs were dead.
- Usage limits: heartbeat extension `lim` {h5,h5t,d7,d7t}, shown on the HUD line and Info page CLAUDE → LIMITS.
- `bridge/`: Claude Code CLI ↔ stick over USB (http hooks + daemon + statusline tee + LaunchAgent). Verified: button A → `allow` round-trip, hooks and rate_limits reach the daemon from a live session.
