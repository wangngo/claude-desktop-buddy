# Changelog (fork wangngo/claude-desktop-buddy)

## 2026-11-18 — branch `m5sticks3`
- Port to M5Unified (`src/hal.h` shim); new default env `m5sticks3` (ESP32-S3); `m5stickc-plus` still builds.
- Fix: M5Unified does not call `Serial.begin` unless `cfg.serial_baudrate` is set; without it USB JSON and logs were dead.
- Usage limits: heartbeat extension `lim` {h5,h5t,d7,d7t}, shown on the HUD line and Info page CLAUDE → LIMITS.
- `bridge/`: Claude Code CLI ↔ stick over USB (http hooks + daemon + statusline tee + LaunchAgent). Verified: button A → `allow` round-trip, hooks and rate_limits reach the daemon from a live session.
