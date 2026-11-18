# TASKS

## Open
- [ ] Verify on device: IMU axes on StickS3 (face-down nap, shake dizzy, clock auto-rotate) against the vendor IMU axis diagram.
- [ ] Verify battery % / charging / usb readout on StickS3 (M5PM1 may not report VBUS/current → info page DEVICE).
- [ ] `pio run -e m5sticks3 -t uploadfs` for the GIF character (bufo) — LittleFS is empty after erase (`mount failed -84` is expected until then).
- [ ] Optional: enable PSRAM (qio_opi, BOARD_HAS_PSRAM) per vendor PlatformIO config.
- [ ] Optional: screen-off on S3 via menu (power-button click resets on M5PM1).
- [ ] Test PermissionRequest from a real non-auto session (only simulated + button round-trip tested so far).

## Done
- [x] Port to M5Unified + StickS3 env, flashed and running (2026-11-18).
- [x] USB bridge for Claude Code CLI + usage limits on device (2026-11-18).
