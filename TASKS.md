# TASKS

## Open
- [ ] Verify battery % / charging / usb readout on StickS3 (M5PM1 may not report VBUS/current → info page DEVICE).
- [ ] `pio run -e m5sticks3 -t uploadfs` for the GIF character (bufo) — LittleFS is empty after erase (`mount failed -84` is expected until then).
- [ ] Optional: enable PSRAM (qio_opi, BOARD_HAS_PSRAM) per vendor PlatformIO config.
- [ ] Optional: screen-off on S3 via menu (power-button click resets on M5PM1).
- [ ] Test PermissionRequest from a real non-auto session (only simulated + button round-trip tested so far).

- [ ] IR Daikin (deferred by W): RX unusable (EXT_5V boost noise ~8.7kHz on G42, see CHANGELOG). Next: W reads remote model (ARC…) → pick IRremoteESP8266 Daikin variant, or trial-send variants and listen for the AC beep. FAQ fix to try: full battery / load on Grove.
- [ ] Mic: push-to-talk voice approve ("được"/"không") — option 2 from the mic/speaker menu (not started).
- [ ] Flash firmware with BLE RX 8 KB (src/ble_bridge.cpp) — needs USB.
- [ ] WiFi network: stick can't see 'Quang4G' (code 1 NO_SSID_AVAIL; 5 GHz?). Waiting for W: a 2.4 GHz SSID (scan saw 'Verizon-SONIM-H500_5D_2.4G' at -19 dBm). Then `curl -XPOST localhost:47830/wifi_set -d '{"ssid":..,"psk":..}'`.
- [ ] Menus/settings/info screens still small portrait type.
- [ ] WiFi (LAN transport + OTA + device HTTP API for AC) — agreed order IR→BT→WiFi, IR deferred.

## Done
- [x] IMU axes verified + gestures (portrait list / landscape dash / face-down DND / shake) (2026-11-18).
- [x] Big-type landscape UI + Clawd (2026-11-18).
- [x] Speaker voice (macOS say Linh) + sfx, PSRAM on (2026-11-18).
- [x] Port to M5Unified + StickS3 env, flashed and running (2026-11-18).
- [x] USB bridge for Claude Code CLI + usage limits on device (2026-11-18).
