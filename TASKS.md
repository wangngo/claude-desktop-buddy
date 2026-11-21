# TASKS

## Open
- [ ] Flash + test deep sleep (`sleepMin`, src/main.cpp auto-off block): default 2 min: unplug USB, stop bridge → sleeps ~2.5 min; BtnA wakes, menu not opened, reset reason `deepsleep`; must NOT sleep on USB / BLE linked / prompt / AP. Then measure battery drop overnight.
- [ ] Fix pre-existing `m5stickc-plus` build break: `src/power_mon.h:93` uses `M5.Power.M5pm1` (S3-only) → guard by board/target.
- [ ] Verify battery % / charging / usb readout on StickS3 (M5PM1 may not report VBUS/current → info page DEVICE).
- [ ] `pio run -e m5sticks3 -t uploadfs` for the GIF character (bufo) — LittleFS is empty after erase (`mount failed -84` is expected until then).
- [ ] Optional: enable PSRAM (qio_opi, BOARD_HAS_PSRAM) per vendor PlatformIO config.
- [ ] Optional: screen-off on S3 via menu (power-button click resets on M5PM1).
- [ ] Test PermissionRequest from a real non-auto session (only simulated + button round-trip tested so far).

- [ ] IR Daikin (deferred by W): RX unusable (EXT_5V boost noise ~8.7kHz on G42, see CHANGELOG). Next: W reads remote model (ARC…) → pick IRremoteESP8266 Daikin variant, or trial-send variants and listen for the AC beep. FAQ fix to try: full battery / load on Grove.
- [ ] Mic: push-to-talk voice approve ("được"/"không") — option 2 from the mic/speaker menu (not started).
- [ ] Flash firmware with BLE RX 8 KB (src/ble_bridge.cpp) — needs USB.
- [ ] WiFi: 'Quang4G' IS on 2.4 GHz (ch11, -40 dBm, WPA2/WPA3 mixed) but joins time out silently (code 6, no disconnect reason). W to check router: MAC filter / client limit / band steering / set WPA2-only, or try phone 2.4 GHz hotspot.
- [ ] Test web UI live (src/web_ui.h) once joined: auth/PIN, lockout, X-Buddy, settings, scan, wifi set, chart, reboot; SoftAP setup flow from menu.
- [ ] IR (paused by W): Daikin variant unknown (8 sent 21:57, no confirmation). Onboard RX unusable (boost noise, USB doesn't feed EXT_5V). Options: camera check of LED, software filter, external VS1838B on HAT 3V3.
- [ ] Audible whine on USB only: ask W result of charge-off window (22:18:24–34); try wall charger / other cable.
- [ ] USB drop/reset on power-state change while on USB (charge re-enable, boost off) — investigate (rawRst via `pm1`).
- [ ] Remove/keep diag cmds (`spk`, `chg`, `bri`, `ir_capture`, wifi_scan `passive`/`txp`) before merging.
- [ ] Menus/settings/info screens still small portrait type.
- [ ] Firmware keeps stale lim when bridge sends no `lim` (both windows expired) — `src/data.h:125`.
- [ ] Firmware `_LineBuf<1024>` (`src/data.h`): heartbeat ~693 B now, Vietnamese entries could exceed 1 KB → line dropped.
- [ ] WiFi (LAN transport + OTA + device HTTP API for AC) — agreed order IR→BT→WiFi, IR deferred.

## Done
- [x] 5h limit flipping between same-account sessions — `pick_rate` merge (2026-11-21).
- [x] Power monitor + battery icon + power.csv (2026-11-18).
- [x] WiFi heat fix (no core auto-reconnect, backoff, radio off, give-up) + TX cap 15 dBm (2026-11-18).
- [x] Web UI + Daikin IR send + IR capture tooling built & flashed (2026-11-18, web untested live).
- [x] IMU axes verified + gestures (portrait list / landscape dash / face-down DND / shake) (2026-11-18).
- [x] Big-type landscape UI + Clawd (2026-11-18).
- [x] Speaker voice (macOS say Linh) + sfx, PSRAM on (2026-11-18).
- [x] Port to M5Unified + StickS3 env, flashed and running (2026-11-18).
- [x] USB bridge for Claude Code CLI + usage limits on device (2026-11-18).
