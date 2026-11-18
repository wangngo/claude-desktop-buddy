# Claude Code CLI bridge (USB serial)

Connects **Claude Code in the terminal** to the stick over USB, without the
Claude desktop app. Claude Code `http` hooks POST events to a small local
daemon, which pushes the heartbeat snapshot from `REFERENCE.md` over the
USB serial port. Permission prompts show up on the stick: **A = allow once,
B = deny**.

```
Claude Code ──http hooks──▶ buddy_bridge.py (127.0.0.1:47830) ──USB CDC──▶ StickS3
statusline ──statusline-tee.sh──┘   (rate_limits → "lim" field)
```

## Pieces

| File | Role |
|---|---|
| `buddy_bridge.py` | daemon: HTTP on `127.0.0.1:47830`, holds the serial port open (each open resets the ESP32-S3) |
| `statusline-tee.sh` | statusline wrapper: forwards the statusline JSON (has `rate_limits`) to `/statusline`, then runs `ccstatusline` unchanged |
| `com.wangngo.claude-buddy-bridge.plist` | LaunchAgent, `KeepAlive`, log `~/Library/Logs/claude-buddy-bridge.log` |

## Transports

USB is used first. Without a cable, the daemon scans BLE for a `Claude*`
device advertising Nordic UART. On the first connect, macOS asks for the
**6-digit passkey shown on the stick**; later connects reuse the bond.
Plugging USB back in switches over to it. Over BLE, speech goes at 8 kHz with ≤2
audio lines in flight (~2× realtime). BLE takes the stick's single BLE
slot, so Claude Desktop's Hardware Buddy can't connect at the same time
(disable with `--ble-prefix ""`).

## Behaviour

- **Permission prompt, stick attached**: the stick shows it and waits up to
  25 s (`--perm-timeout`). No button press → the normal terminal prompt
  appears. During the wait the terminal prompt is not shown.
- **Stick unplugged**: the daemon answers at once, the terminal prompt shows as usual.
- **Daemon down**: the hooks fail non-blocking, so Claude Code keeps working.
- **Usage limits**: 5h / 7d `used_percentage` and reset time, read from the
  statusline JSON (Pro/Max only, after the session's first API call). They are shown
  in the HUD line above the transcript and on Info page "CLAUDE" → LIMITS.
  The `lim` field is a bridge extension that stock firmware ignores.

- **Voice and sounds**: the stick's speaker plays short tones (`sfx`: done /
  warn / error / prompt) and Vietnamese speech made offline by the macOS
  voice `say -v Linh`. The PCM goes over USB in acked base64 chunks into
  PSRAM. Events:
  - a permission prompt says "Claude xin …";
  - the end of a turn plays the done chime, plus "Xong rồi" if the turn ran ≥20 s;
  - the 5h/7d limit crossing 80/90/95% plays a warn tone and speech, once per window.

  Muted when the stick's *sound* setting is off; turn it off on the daemon with
  `--no-voice`. Test: `curl -XPOST localhost:47830/say -d '{"text":"xin chào"}'`
  or `-d '{"sfx":"done"}'`.

- **Gestures (stick IMU)**:
  - stand upright → portrait list of recent commands;
  - lay sideways → landscape dashboard;
  - face down → do-not-disturb: screen off, muted, and prompts stay in the terminal;
  - shake → dizzy Clawd.
- **WiFi**: provision once with `curl -XPOST localhost:47830/wifi_set -d '{"ssid":"…","psk":"…"}'`
  (2.4 GHz only). The key lives in `~/.config/claude-buddy/state.json` (0600).
  Traffic is HMAC-authenticated but **not encrypted** on the LAN.

Health/debug: `curl -s http://127.0.0.1:47830/ | python3 -m json.tool`

## Install (macOS)

```bash
cd bridge && uv venv .venv && uv pip install --python .venv/bin/python pyserial
cp com.wangngo.claude-buddy-bridge.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.wangngo.claude-buddy-bridge.plist
```

Hooks (append to `~/.claude/settings.local.json` → `hooks`), each
`{"type":"http","url":"http://127.0.0.1:47830/"}`: `SessionStart`,
`SessionEnd`, `UserPromptSubmit`, `Stop`, `PreToolUse` (matcher `*`),
`PermissionRequest` (matcher `*`, `timeout: 30`, `onFailure: "continue"`). Also `StopFailure`
(API error at the end of a turn → error tone + "Claude gặp lỗi").
Statusline: point `statusLine.command` in `~/.claude/settings.json` at
`statusline-tee.sh`. The real command defaults to `~/.local/bin/ccstatusline`
and can be overridden with `BUDDY_REAL_STATUSLINE`.

## Uninstall

```bash
launchctl bootout gui/$(id -u)/com.wangngo.claude-buddy-bridge
rm ~/Library/LaunchAgents/com.wangngo.claude-buddy-bridge.plist
```
Then remove the hook entries whose url is `http://127.0.0.1:47830/` from
`settings.local.json` and restore `statusLine.command` (the backups
`settings*.json.bak-buddy-<timestamp>` sit next to them).

## Gotchas

- Opening `/dev/cu.usbmodem*` on macOS resets the StickS3. Never open it from
  a per-event script; only the daemon holds it. Stop the LaunchAgent before
  `pio run -t upload`/`uploadfs` (the port is busy), then start it again with
  `launchctl kickstart gui/$(id -u)/com.wangngo.claude-buddy-bridge`. Wait ~2 s after
  `bootout`: the old process needs a moment to release the port, otherwise upload fails.
- The daemon handshakes after open (waits for the `buddy:` boot line, then
  polls `owner` until it is acked). A fixed sleep lost the first command
  once PSRAM init made boot slower.
- StickS3 side button is the M5PM1 power key. Single click resets, double
  click powers off, and holding ~2 s (green LED blinks) enters download mode.
