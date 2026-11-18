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
import argparse, glob, json, logging, threading, time, uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

log = logging.getLogger("buddy")

# Tools whose permission "allow" needs updatedInput (an answer / plan) — the
# stick can't supply that, so they always go to the terminal.
TERMINAL_ONLY_TOOLS = {"AskUserQuestion", "ExitPlanMode"}
NONINTERACTIVE_MODES = {"dontAsk", "bypassPermissions"}
SESSION_STALE_S = 30 * 60   # forget sessions with no event for this long
KEEPALIVE_S = 10            # REFERENCE.md: device treats >30s silence as dead


class Prompt:
    def __init__(self, pid, session, tool, hint):
        self.id, self.session, self.tool, self.hint = pid, session, tool, hint
        self.event = threading.Event()
        self.decision = None
        self.shown = threading.Event()   # set when it reaches the head of the queue


class Bridge:
    def __init__(self, port_glob, owner, tz_offset):
        self.port_glob, self.owner, self.tz = port_glob, owner, tz_offset
        self.lock = threading.Lock()
        self.ser = None
        self.ser_lock = threading.Lock()  # serialises write/close of the port
        self.sessions = {}        # session_id -> {"running": bool, "seen": ts}
        self.entries = []         # newest first
        self.msg = "Claude Code idle"
        self.prompts = []         # FIFO of pending Prompt
        self.rate = {}            # statusline rate_limits, latest seen
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
        log.info("opened %s, waiting for stick to boot", ports[0])
        time.sleep(4)  # open resets the chip; let setup() finish
        s.reset_input_buffer()
        with self.ser_lock:
            self.ser = s
        self._write({"time": [int(time.time()), self.tz]})
        if self.owner:
            self._write({"cmd": "owner", "name": self.owner})
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

    def serial_loop(self):
        buf = b""
        while True:
            if not self.ser:
                try:
                    if not self._open():
                        time.sleep(2); continue
                except Exception as e:
                    log.warning("open failed: %s", e); time.sleep(2); continue
            s = self.ser
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
            elif name in ("Stop", "StopFailure"):
                self._touch(sid, running=False)
                self.msg = "done" if name == "Stop" else "stopped (error)"
            elif name == "PreToolUse":
                self._touch(sid, running=True)
                self._entry(f"{ev.get('tool_name','?')}: {hint_for(ev)}")
                self.msg = ev.get("tool_name", "tool")
            else:
                self._touch(sid)
        self.dirty.set()

    def on_statusline(self, st):
        rl = st.get("rate_limits")
        if isinstance(rl, dict):
            with self.lock:
                changed = rl != self.rate
                self.rate = rl
            if changed:
                self.dirty.set()

    def on_permission(self, ev, timeout):
        sid = ev.get("session_id", "?")
        tool = ev.get("tool_name", "?")
        if (not self.ser                                   # stick not attached
                or tool in TERMINAL_ONLY_TOOLS             # needs updatedInput
                or ev.get("agent_id")                      # subagent / teammate
                or ev.get("permission_mode") in NONINTERACTIVE_MODES):
            return None  # let Claude Code's own flow decide
        p = Prompt("p" + uuid.uuid4().hex[:6], sid, tool, hint_for(ev))
        with self.lock:
            self._touch(sid, running=True)
            self.prompts.append(p)
        self.dirty.set()
        log.info("prompt %s %s: %s", p.id, p.tool, p.hint)
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
            self._reply({"serial": bool(bridge.ser), "rate_limits": bridge.rate, **bridge.snapshot()})
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem*")
    ap.add_argument("--listen", default="127.0.0.1:47830")
    ap.add_argument("--owner", default="")
    ap.add_argument("--perm-timeout", type=float, default=25.0)
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.v else logging.INFO,
                        format="%(asctime)s %(message)s")
    tz = -time.altzone if time.localtime().tm_isdst else -time.timezone
    b = Bridge(a.port, a.owner, tz)
    threading.Thread(target=b.serial_loop, daemon=True).start()
    threading.Thread(target=b.heartbeat_loop, daemon=True).start()
    host, port = a.listen.rsplit(":", 1)
    srv = ThreadingHTTPServer((host, int(port)), make_handler(b, a.perm_timeout))
    log.info("listening on http://%s", a.listen)
    srv.serve_forever()


if __name__ == "__main__":
    main()
