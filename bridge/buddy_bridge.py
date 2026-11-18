#!/usr/bin/env python3
"""Claude Code CLI <-> Hardware Buddy bridge over USB serial.

Claude Code `http` hooks POST their event JSON to this daemon. The daemon
keeps a tally of sessions and pushes the heartbeat snapshot described in
REFERENCE.md to the stick. PermissionRequest hooks block until the stick
answers (A = once, B = deny) or a timeout, then fall back to the normal
terminal prompt by returning an empty decision.

The serial port is opened once and kept open: on macOS every open of the
ESP32-S3 USB-Serial-JTAG port resets the chip.
"""
import argparse, base64, glob, hashlib, hmac, json, secrets, socket, logging, os, queue, subprocess, tempfile, threading, time, uuid, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import asyncio
import serial
try:
    from bleak import BleakClient, BleakScanner
except ImportError:          # BLE optional
    BleakClient = BleakScanner = None

log = logging.getLogger("buddy")

# Tools whose permission "allow" needs updatedInput (an answer / plan) — the
# stick can't supply that, so they always go to the terminal.
TERMINAL_ONLY_TOOLS = {"AskUserQuestion", "ExitPlanMode"}
NONINTERACTIVE_MODES = {"dontAsk", "bypassPermissions"}
VOICE = "Linh"               # macOS built-in vi_VN voice (offline, free)
AUDIO_RATE = 16000
AUDIO_MAX_S = 8
CHUNK_RAW = 600              # bytes per audio line (base64 800 < device 1024 line buf)
CHUNK_WINDOW = 8             # un-acked chunks in flight
LONG_TURN_S = 20             # speak "xong" only for turns longer than this
LIMIT_THRESHOLDS = (80, 90, 95)
TOOL_VN = {"Bash": "chạy lệnh", "Edit": "sửa file", "Write": "ghi file",
           "MultiEdit": "sửa file", "NotebookEdit": "sửa notebook",
           "WebFetch": "truy cập web", "WebSearch": "tìm trên web"}
SESSION_STALE_S = 30 * 60   # forget sessions with no event for this long
KEEPALIVE_S = 10            # REFERENCE.md: device treats >30s silence as dead


NUS_SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # we write
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # stick notifies


class BleLink:
    """Nordic UART over BLE, driven by a private asyncio loop thread.
    Exposes write()/close() like a pyserial port so the Bridge can treat both
    transports the same. First connect triggers macOS pairing (passkey shown
    on the stick); later connects reuse the bond."""
    kind = "ble"

    def __init__(self, on_line):
        self.on_line = on_line
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()
        self.client = None
        self.buf = b""
        self.name = None

    def _run(self, coro, timeout):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def _notify(self, _h, data):
        self.buf += bytes(data)
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            self.on_line(line.strip())

    async def _connect(self, prefix):
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: (d.name or "").startswith(prefix)
            or NUS_SVC in [u.lower() for u in adv.service_uuids], timeout=6)
        if not dev:
            return False
        c = BleakClient(dev, disconnected_callback=lambda _c: None)
        await c.connect(timeout=15)
        await c.start_notify(NUS_TX, self._notify)   # encrypted CCCD -> pairing
        self.client, self.name = c, dev.name
        return True

    def connect(self, prefix="Claude"):
        return self._run(self._connect(prefix), 120)   # pairing dialog may take a while

    @property
    def connected(self):
        return bool(self.client and self.client.is_connected)

    def write(self, data):
        if not self.connected:
            raise IOError("ble disconnected")
        n = 180   # macOS negotiates MTU 185
        for i in range(0, len(data), n):
            # no per-packet response: line-level acks + the stick's 8 KB ring
            # buffer provide flow control (bridge keeps <=2 audio lines in flight)
            self._run(self.client.write_gatt_char(NUS_RX, data[i:i + n], response=False), 10)

    def close(self):
        if self.client:
            try:
                self._run(self.client.disconnect(), 10)
            except Exception:
                pass
        self.client = None


CONF_DIR = os.path.expanduser("~/.config/claude-buddy")
WIFI_PORT = 47831


def conf_load():
    try:
        with open(os.path.join(CONF_DIR, "state.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def conf_save(d):
    os.makedirs(CONF_DIR, mode=0o700, exist_ok=True)
    p = os.path.join(CONF_DIR, "state.json")
    fd = os.open(p + ".tmp", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(d, f)
    os.replace(p + ".tmp", p)


class WifiLink:
    """TCP to the stick's WiFi server with HMAC challenge-response auth."""
    kind = "wifi"

    def __init__(self, on_line):
        self.on_line = on_line
        self.sock = None
        self.name = None

    def connect(self, hosts, key_hex):
        key = bytes.fromhex(key_hex)
        for h in hosts:
            if not h:
                continue
            try:
                sk = socket.create_connection((h, WIFI_PORT), timeout=3)
            except OSError:
                continue
            sk.settimeout(5)
            f = sk.makefile("rb")
            try:
                hello = json.loads(f.readline())
                mac = hmac.new(key, hello["nonce"].encode(), hashlib.sha256).hexdigest()
                sk.sendall((json.dumps({"cmd": "auth", "mac": mac}) + "\n").encode())
                ack = json.loads(f.readline())
                if not ack.get("ok"):
                    raise IOError("auth rejected")
            except Exception as e:
                log.warning("wifi auth with %s failed: %s", h, e)
                sk.close()
                continue
            sk.settimeout(None)
            self.sock, self.name = sk, h
            threading.Thread(target=self._reader, args=(f,), daemon=True).start()
            return True
        return False

    def _reader(self, f):
        try:
            for line in f:
                self.on_line(line.strip())
        except Exception:
            pass
        self.sock = None

    @property
    def connected(self):
        return self.sock is not None

    def write(self, data):
        if not self.sock:
            raise IOError("wifi disconnected")
        self.sock.sendall(data)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None


class Prompt:
    def __init__(self, pid, session, tool, hint):
        self.id, self.session, self.tool, self.hint = pid, session, tool, hint
        self.event = threading.Event()
        self.decision = None
        self.shown = threading.Event()   # set when it reaches the head of the queue


class Bridge:
    def __init__(self, port_glob, owner, tz_offset, ble_prefix="Claude"):
        self.port_glob, self.owner, self.tz = port_glob, owner, tz_offset
        self.ble_prefix = ble_prefix     # "" disables BLE
        self.lock = threading.Lock()
        self.ser = None
        self.ser_lock = threading.Lock()  # serialises write/close of the port
        self.sessions = {}        # session_id -> {"running": bool, "seen": ts}
        self.entries = []         # newest first
        self.msg = "Claude Code idle"
        self.prompts = []         # FIFO of pending Prompt
        self.rate = {}            # rate_limits shown on the stick
        self.rate_by_sess = {}    # session_id -> (rate_limits, time seen)
        self.last_active = {}     # session_id -> time of last hook activity
        self.voice = True
        self.dnd = False          # stick face-down: do-not-disturb
        self.audio_q = queue.Queue()
        self.ack_cv = threading.Condition()
        self.acks = {}            # ack name -> (count, last payload)
        self.turn_start = {}      # session -> time of UserPromptSubmit
        self.limit_said = set()   # (window, resets_at, threshold)
        self.dirty = threading.Event()

    # ---- serial ----
    def _open(self):
        ports = sorted(glob.glob(self.port_glob))
        if not ports:
            return False
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = ports[0], 115200, 0.2
        s.dtr = False; s.rts = False
        s.open()
        try:
            return self._handshake(s, ports[0])
        except Exception:
            s.close()
            raise

    def _handshake(self, s, port):
        log.info("opened %s, waiting for stick to boot", port)
        # Opening the port resets the chip and boot time varies (PSRAM init),
        # so a fixed sleep loses the first commands. Handshake instead: wait
        # for setup()'s "buddy:" line, then poll owner until it is acked.
        buf, end = b"", time.time() + 15
        while time.time() < end and b"buddy:" not in buf:
            buf += s.read(256)
        ready = False
        # owner only when configured: an empty name would wipe the stored one
        probe, want = (({"cmd": "owner", "name": self.owner}, b'"ack":"owner"') if self.owner
                       else ({"cmd": "sfx", "name": "none"}, b'"ack":"sfx"'))
        for _ in range(10):
            s.write((json.dumps(probe) + "\n").encode())
            t = time.time() + 1.0
            while time.time() < t:
                buf += s.read(256)
                if want in buf:
                    ready = True; break
            if ready:
                break
        if not ready:
            log.warning("stick did not answer handshake; closing")
            s.close()
            time.sleep(2)
            return False
        with self.ser_lock:
            self.ser = s
        log.info("stick ready")
        self._write({"time": [int(time.time()), self.tz]})
        self.dirty.set()
        return True

    def _write(self, obj):
        data = (json.dumps(obj, ensure_ascii=False) + "\n").encode()
        with self.ser_lock:
            s = self.ser
            if not s:
                return
            try:
                s.write(data)
                return
            except Exception as e:
                log.warning("write failed: %s", e)
        self._drop(s)

    def _drop(self, s):
        """Close `s` only if it is still the current port; release waiters."""
        with self.ser_lock:
            if self.ser is not s:
                return
            try:
                s.close()
            except Exception:
                pass
            self.ser = None
        with self.lock:
            for p in self.prompts:   # stick gone: fall back to terminal now
                p.event.set()

    def _wifi_open(self):
        c = conf_load()
        if not c.get("key") or not (c.get("host") or c.get("ip")):
            return False
        link = WifiLink(self._on_device_line)
        hosts = [c.get("host") and c["host"] + ".local", c.get("ip")]
        if not link.connect(hosts, c["key"]):
            return False
        with self.ser_lock:
            self.ser = link
        log.info("stick ready over WiFi (%s)", link.name)
        self._write({"time": [int(time.time()), self.tz]})
        if self.owner:
            self._write({"cmd": "owner", "name": self.owner})
        self.dirty.set()
        return True

    POWER_CSV = os.path.join(CONF_DIR, "power.csv")
    POWER_COLS = ("ts", "up", "mV", "pct", "chg", "tC", "wifi", "bri", "rst")

    def _log_power(self, m):
        """Append one {"evt":"pwr"} sample from the stick to power.csv."""
        try:
            new = not os.path.exists(self.POWER_CSV)
            with open(self.POWER_CSV, "a") as f:
                if new:
                    f.write(",".join(self.POWER_COLS) + "\n")
                row = [time.strftime("%Y-%m-%d %H:%M:%S")] + [str(m.get(k, "")) for k in self.POWER_COLS[1:]]
                f.write(",".join(row) + "\n")
        except OSError as e:
            log.warning("power.csv: %s", e)

    def _reset_link_state(self):
        self.dnd = False

    def wifi_provision(self, ssid, psk):
        """Send WiFi creds + auth key to the stick over the current link."""
        if not self.ser:
            return {"ok": False, "error": "stick not connected"}
        c = conf_load()
        c.setdefault("key", secrets.token_hex(32))
        conf_save(c)
        with self.ack_cv:
            base = self.acks.get("wifi_set", (0, None))[0]
        self._write({"cmd": "wifi_set", "ssid": ssid, "psk": psk, "key": c["key"]})
        a = self._wait_ack("wifi_set", base + 1, 5)
        if not a or not a.get("ok"):
            return {"ok": False, "error": f"stick refused: {a}"}
        # wait for the stick to join and report its address
        for _ in range(20):
            time.sleep(1)
            with self.ack_cv:
                base = self.acks.get("wifi_status", (0, None))[0]
            self._write({"cmd": "wifi_status"})
            st = self._wait_ack("wifi_status", base + 1, 3) or {}
            if st.get("ip"):
                c.update(host=st.get("host"), ip=st.get("ip"))
                conf_save(c)
                return {"ok": True, "host": st.get("host"), "ip": st["ip"], "rssi": st.get("rssi")}
        return {"ok": False, "error": "stick did not join WiFi in 20s", "last": st}

    def _ble_open(self):
        if not (self.ble_prefix and BleakClient):
            return False
        link = BleLink(self._on_device_line)
        try:
            if not link.connect(self.ble_prefix):
                return False
            probe, want = (({"cmd": "owner", "name": self.owner}, "owner") if self.owner
                           else ({"cmd": "sfx", "name": "none"}, "sfx"))
            with self.ack_cv:
                base = self.acks.get(want, (0, None))[0]
            for _ in range(5):
                link.write((json.dumps(probe) + "\n").encode())
                if self._wait_ack(want, base + 1, 2.0):
                    break
            else:
                raise IOError("no handshake ack over BLE")
        except Exception as e:
            log.warning("ble connect failed: %s", e)
            link.close()
            return False
        with self.ser_lock:
            self.ser = link
        log.info("stick ready over BLE (%s)", link.name)
        self._write({"time": [int(time.time()), self.tz]})
        self.dirty.set()
        return True

    def serial_loop(self):
        buf = b""
        while True:
            if not self.ser:
                try:
                    if not self._open() and not self._wifi_open() and not self._ble_open():
                        time.sleep(3); continue
                except Exception as e:
                    log.warning("open failed: %s", e); time.sleep(2); continue
            s = self.ser
            if isinstance(s, (BleLink, WifiLink)):
                # reads happen on the link's own thread; here we only watch
                # for link loss, or a better link (USB > WiFi > BLE).
                time.sleep(1)
                if not s.connected:
                    log.info("%s link lost", s.kind); self._drop(s)
                elif glob.glob(self.port_glob):
                    log.info("USB attached; switching from %s", s.kind); self._drop(s)
                elif isinstance(s, BleLink) and int(time.time()) % 30 == 0 and conf_load().get("key"):
                    probe = WifiLink(lambda l: None)
                    c = conf_load()
                    if probe.connect([c.get("host") and c["host"] + ".local", c.get("ip")], c["key"]):
                        probe.close()
                        log.info("WiFi reachable; switching from BLE"); self._drop(s)
                continue
            try:
                chunk = s.read(256)
            except Exception as e:
                log.warning("read failed: %s", e); self._drop(s); continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._on_device_line(line.strip())

    def _on_device_line(self, line):
        if not line.startswith(b"{"):
            if line: log.debug("dev: %s", line.decode(errors="replace"))
            return
        try:
            m = json.loads(line)
        except ValueError:
            return
        if "ack" in m:
            with self.ack_cv:
                c, _ = self.acks.get(m["ack"], (0, None))
                self.acks[m["ack"]] = (c + 1, m)
                self.ack_cv.notify_all()
            return
        if m.get("evt") == "pwr":
            self._log_power(m)
            return
        if m.get("evt") == "dnd":
            self.dnd = bool(m.get("on"))
            log.info("do-not-disturb %s", "on" if self.dnd else "off")
            return
        if m.get("cmd") == "permission":
            with self.lock:
                for p in self.prompts:
                    if p.id == m.get("id"):
                        p.decision = m.get("decision")
                        p.event.set()
                        log.info("device decided %s -> %s", p.id, p.decision)
                        break

    # ---- heartbeat ----
    def snapshot(self):
        with self.lock:
            now = time.time()
            for sid in [k for k, v in self.sessions.items() if now - v["seen"] > SESSION_STALE_S]:
                del self.sessions[sid]
            snap = {
                "total": len(self.sessions),
                "running": sum(1 for v in self.sessions.values() if v["running"]),
                "waiting": len(self.prompts),
                "msg": self.msg,
                "entries": self.entries[:6],
            }
            lim = limits_for(self.rate, now)
            if lim:
                snap["lim"] = lim
            if self.prompts:
                p = self.prompts[0]
                p.shown.set()
                snap["prompt"] = {"id": p.id, "tool": p.tool[:19], "hint": p.hint[:43]}
                snap["msg"] = f"approve: {p.tool}"
            return snap

    def heartbeat_loop(self):
        while True:
            self.dirty.wait(KEEPALIVE_S)
            self.dirty.clear()
            self._write(self.snapshot())

    # ---- hook events ----
    def _touch(self, sid, **kw):
        s = self.sessions.setdefault(sid, {"running": False, "seen": 0})
        s["seen"] = time.time()
        self.last_active[sid] = s["seen"]
        s.update(kw)

    def _entry(self, text):
        self.entries.insert(0, time.strftime("%H:%M ") + text[:80])
        del self.entries[12:]

    def on_event(self, ev):
        name = ev.get("hook_event_name", "")
        sid = ev.get("session_id", "?")
        with self.lock:
            if name == "SessionStart":
                self._touch(sid, running=False)
                self.msg = "session started"
            elif name == "SessionEnd":
                self.sessions.pop(sid, None)
                self.msg = "session ended"
            elif name == "UserPromptSubmit":
                self._touch(sid, running=True)
                self.msg = "working..."
                self.turn_start[sid] = time.time()
            elif name in ("Stop", "StopFailure"):
                self._touch(sid, running=False)
                self.msg = "done" if name == "Stop" else "stopped (error)"
                took = time.time() - self.turn_start.pop(sid, time.time())
                if name == "StopFailure":
                    self.sfx("error"); self.say("Claude gặp lỗi")
                elif not ev.get("agent_id"):
                    self.sfx("done")
                    if took >= LONG_TURN_S:
                        self.say("Xong rồi")
            elif name == "PreToolUse":
                self._touch(sid, running=True)
                self._entry(f"{ev.get('tool_name','?')}: {hint_for(ev)}")
                self.msg = ev.get("tool_name", "tool")
            else:
                self._touch(sid)
        self.dirty.set()

    # ---- audio ----
    def sfx(self, name):
        if self.voice:
            self.audio_q.put(("sfx", name))

    def say(self, text):
        if self.voice:
            self.audio_q.put(("say", text))

    def _wait_ack(self, name, count, timeout=3.0):
        end = time.time() + timeout
        with self.ack_cv:
            while self.acks.get(name, (0, None))[0] < count:
                left = end - time.time()
                if left <= 0:
                    return None
                self.ack_cv.wait(left)
            return self.acks[name][1]

    def _send_audio(self, pcm, rate=AUDIO_RATE):
        def acked(name):
            with self.ack_cv:
                return self.acks.get(name, (0, None))[0]
        base = acked("audio_begin")
        self._write({"cmd": "audio_begin", "rate": rate, "len": len(pcm)})
        a = self._wait_ack("audio_begin", base + 1)
        if not a or not a.get("ok"):
            log.info("audio_begin refused (%s)", a)   # muted on device, or no RAM
            return False
        chunks = [pcm[i:i + CHUNK_RAW] for i in range(0, len(pcm), CHUNK_RAW)]
        base = acked("audio")
        for i, c in enumerate(chunks):
            win = 2 if isinstance(self.ser, BleLink) else CHUNK_WINDOW   # USB/WiFi: 8
            if i >= win:
                a = self._wait_ack("audio", base + i - win + 1, 6.0)
                if a is None or not a.get("ok"):
                    log.warning("audio chunk %d/%d failed: %s", i, len(chunks), a); return False
            self._write({"cmd": "audio", "d": base64.b64encode(c).decode()})
        a = self._wait_ack("audio", base + len(chunks))
        if a is None or not a.get("ok") or a.get("n") != len(pcm):
            log.warning("audio incomplete: %s (want n=%d)", a, len(pcm)); return False
        self._write({"cmd": "audio_end"})
        return True

    def audio_loop(self):
        while True:
            kind, arg = self.audio_q.get()
            for _ in range(30):              # (re)connecting: wait, don't drop
                if self.ser:
                    break
                time.sleep(0.5)
            if not self.ser:
                log.info("audio dropped (no stick): %s %r", kind, arg)
                continue
            try:
                if kind == "sfx":
                    self._write({"cmd": "sfx", "name": arg})
                    time.sleep(0.4)
                else:
                    t0 = time.time()
                    rate = 8000 if isinstance(self.ser, BleLink) else AUDIO_RATE
                    pcm = tts_pcm(arg, rate)
                    if self._send_audio(pcm, rate):
                        log.info("said %r via %s (%.1fs audio, %.1fs total)", arg,
                                 getattr(self.ser, "kind", "usb"), len(pcm) / 2 / rate, time.time() - t0)
                        time.sleep(len(pcm) / 2 / rate)   # don't overlap clips
            except Exception as e:
                log.warning("audio %s failed: %s", kind, e)

    def on_statusline(self, st):
        rl = st.get("rate_limits")
        if isinstance(rl, dict):
            sid = st.get("session_id", "?")
            with self.lock:
                now = time.time()
                self.rate_by_sess[sid] = (rl, now)
                for k in [k for k, (_, t) in self.rate_by_sess.items() if now - t > 600]:
                    del self.rate_by_sess[k]
                # Several sessions may belong to different accounts, each with
                # its own limits. Show the session that was active most recently
                # (hook activity), not whichever statusline refreshed last.
                best = max(self.rate_by_sess,
                           key=lambda k: (self.last_active.get(k, 0), self.rate_by_sess[k][1]))
                rl = self.rate_by_sess[best][0]
                changed = rl != self.rate
                self.rate = rl
            if changed:
                self.dirty.set()
                self._check_limits(rl)

    def _check_limits(self, rl):
        names = {"five_hour": "5 giờ", "seven_day": "7 ngày"}
        for key, vn in names.items():
            w = rl.get(key)
            if not isinstance(w, dict) or w.get("used_percentage") is None:
                continue
            pct = float(w["used_percentage"])
            crossed = [t for t in LIMIT_THRESHOLDS if pct >= t]
            if not crossed:
                continue
            tag = (key, w.get("resets_at"), crossed[-1])
            with self.lock:
                if tag in self.limit_said:
                    continue
                # mark every lower threshold too, so we only announce the highest
                for t in crossed:
                    self.limit_said.add((key, w.get("resets_at"), t))
            self.sfx("warn")
            self.say(f"Hạn mức {vn} đã dùng {int(pct)} phần trăm")

    def on_permission(self, ev, timeout):
        sid = ev.get("session_id", "?")
        tool = ev.get("tool_name", "?")
        if (not self.ser                                   # stick not attached
                or tool in TERMINAL_ONLY_TOOLS             # needs updatedInput
                or ev.get("agent_id")                      # subagent / teammate
                or ev.get("permission_mode") in NONINTERACTIVE_MODES
                or self.dnd):                              # face-down: ask in terminal
            return None  # let Claude Code's own flow decide
        p = Prompt("p" + uuid.uuid4().hex[:6], sid, tool, hint_for(ev))
        with self.lock:
            self._touch(sid, running=True)
            self.prompts.append(p)
        self.dirty.set()
        log.info("prompt %s %s: %s", p.id, p.tool, p.hint)
        self.say("Claude xin " + TOOL_VN.get(tool, "dùng " + tool))
        # The clock starts when the stick actually shows this prompt, so a
        # prompt queued behind another still gets its full window.
        deadline = time.time() + timeout * 3      # hard cap while queued
        while not p.shown.wait(0.2):
            if p.event.is_set() or time.time() > deadline:
                break
        if not p.event.is_set():
            p.event.wait(max(0.0, min(timeout, deadline - time.time())))
        with self.lock:
            self.prompts.remove(p)
            if p.decision:
                self._entry(f"{'OK' if p.decision == 'once' else 'NO'} {p.tool}")
        self.dirty.set()
        return p.decision


VN_DOW = ["T2", "T3", "T4", "T5", "T6", "T7", "CN"]


def limits_for(rl, now):
    """rate_limits from the statusline JSON -> compact 'lim' object.
    Windows whose resets_at already passed are dropped (Claude Code does the same)."""
    out = {}
    for key, short in (("five_hour", "h5"), ("seven_day", "d7")):
        w = rl.get(key) if isinstance(rl, dict) else None
        if not isinstance(w, dict) or w.get("used_percentage") is None:
            continue
        r = w.get("resets_at")
        if isinstance(r, (int, float)) and r <= now:
            continue
        out[short] = int(round(min(max(float(w["used_percentage"]), 0), 100)))
        if isinstance(r, (int, float)):
            t = time.localtime(r)
            out[short + "t"] = (time.strftime("%H:%M", t) if short == "h5"
                                else f"{VN_DOW[t.tm_wday]} {time.strftime('%H:%M', t)}")
    return out


def tts_pcm(text, rate=AUDIO_RATE):
    """macOS `say` -> raw int16 LE mono PCM at `rate`."""
    fd, path = tempfile.mkstemp(suffix=".wav"); os.close(fd)
    try:
        subprocess.run(["/usr/bin/say", "-v", VOICE, "-o", path,
                        "--file-format=WAVE", f"--data-format=LEI16@{rate}", text],
                       check=True, timeout=20, capture_output=True)
        with wave.open(path, "rb") as w:
            return w.readframes(min(w.getnframes(), rate * AUDIO_MAX_S))
    finally:
        os.unlink(path)


def hint_for(ev):
    ti = ev.get("tool_input") or {}
    for k in ("command", "file_path", "url", "pattern", "query", "prompt", "description"):
        if isinstance(ti.get(k), str) and ti[k]:
            return " ".join(ti[k].split())
    for v in ti.values():
        if isinstance(v, str) and v:
            return " ".join(v.split())
    return ""


def make_handler(bridge, perm_timeout):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _reply(self, obj):
            body = json.dumps(obj).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            try:
                n = int(self.headers.get("Content-Length", 0))
                ev = json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                return self._reply({})
            if self.path.startswith("/imu"):
                with bridge.ack_cv:
                    base = bridge.acks.get("imu", (0, None))[0]
                bridge._write({"cmd": "imu"})
                return self._reply(bridge._wait_ack("imu", base + 1, 3) or {"ok": False})
            if self.path.startswith("/wifi_set"):
                return self._reply(bridge.wifi_provision(str(ev.get("ssid", "")), str(ev.get("psk", ""))))
            if self.path.startswith("/say"):
                if ev.get("sfx"): bridge.sfx(ev["sfx"])
                if ev.get("text"): bridge.say(str(ev["text"])[:200])
                return self._reply({})
            if self.path.startswith("/statusline"):
                bridge.on_statusline(ev)
                return self._reply({})
            if ev.get("hook_event_name") == "PermissionRequest":
                d = bridge.on_permission(ev, perm_timeout)
                if d == "once":
                    return self._reply({"hookSpecificOutput": {
                        "hookEventName": "PermissionRequest",
                        "decision": {"behavior": "allow"}}})
                if d == "deny":
                    return self._reply({"hookSpecificOutput": {
                        "hookEventName": "PermissionRequest",
                        "decision": {"behavior": "deny",
                                     "message": "Denied from Hardware Buddy (button B)."}}})
                return self._reply({})  # no answer -> normal terminal prompt
            bridge.on_event(ev)
            self._reply({})

        def do_GET(self):  # health / debug
            link = bridge.ser
            self._reply({"serial": bool(link), "link": getattr(link, "kind", "usb") if link else None,
                         "rate_by_session": {k[:8]: {w: v.get(w, {}).get("used_percentage") for w in ("five_hour", "seven_day")}
                                             for k, (v, _) in bridge.rate_by_sess.items()},
                         "rate_limits": bridge.rate, **bridge.snapshot()})
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem*")
    ap.add_argument("--listen", default="127.0.0.1:47830")
    ap.add_argument("--owner", default="")
    ap.add_argument("--perm-timeout", type=float, default=25.0)
    ap.add_argument("--ble-prefix", default="Claude", help='BLE name prefix; "" disables BLE')
    ap.add_argument("--no-voice", action="store_true", help="no sfx / speech")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.v else logging.INFO,
                        format="%(asctime)s %(message)s")
    tz = -time.altzone if time.localtime().tm_isdst else -time.timezone
    b = Bridge(a.port, a.owner, tz, a.ble_prefix)
    threading.Thread(target=b.serial_loop, daemon=True).start()
    threading.Thread(target=b.heartbeat_loop, daemon=True).start()
    b.voice = not a.no_voice
    threading.Thread(target=b.audio_loop, daemon=True).start()
    host, port = a.listen.rsplit(":", 1)
    srv = ThreadingHTTPServer((host, int(port)), make_handler(b, a.perm_timeout))
    log.info("listening on http://%s", a.listen)
    srv.serve_forever()


if __name__ == "__main__":
    main()
