#!/usr/bin/env python3
"""Raw IR capture from the StickS3 receiver (G42) + host-side glitch filter.

Usage (bridge must be stopped — it owns the serial port):
  tools/ir_capture.py capture --label remote --ms 6000 [--say "Bấm remote"]
  tools/ir_capture.py analyze FILE [--min-us 150]

The firmware streams every edge as uint32 (micros & ~1) | level. The boost
on EXT_5V couples ~8.7 kHz noise (~57 us pulses) onto the receiver output;
real demodulated IR pulses are >= ~300 us, so segments shorter than --min-us
are merged into their neighbours before looking for frames.
"""
import argparse, base64, json, os, struct, subprocess, sys, time

OUT_DIR = os.path.expanduser("~/.config/claude-buddy/ircap")


def capture(port, ms, label, say, extra=None):
    import serial
    s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.2
    s.dtr = False; s.rts = False; s.open()
    time.sleep(6)                      # opening the port resets the stick (rawRst 0x15)
    s.reset_input_buffer()
    if say:
        subprocess.run(["say", "-v", "Linh", say])
    s.write((json.dumps({"cmd": "ir_capture", "ms": ms, **(extra or {})}) + "\n").encode())
    hdr, raw, end = None, bytearray(), time.time() + ms / 1000 + 30
    while time.time() < end:
        l = s.readline().decode(errors="replace").strip()
        if not l.startswith("{"):
            continue
        m = json.loads(l)
        if m.get("ack") == "ir_capture":
            hdr = m
        elif "ircap" in m:
            raw += base64.b64decode(m["ircap"])
        elif "ircap_end" in m:
            break
    s.close()
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, f"{time.strftime('%Y%m%d-%H%M%S')}-{label}.bin")
    with open(path, "wb") as f:
        f.write(raw)
    print("header:", hdr, "saved:", path, len(raw) // 4, "edges")
    return path


def segments(raw):
    """[(level, duration_us)] — level held for duration before the next edge."""
    v = struct.unpack(f"<{len(raw)//4}I", raw)
    out = []
    for a, b in zip(v, v[1:]):
        out.append((a & 1, ((b & ~1) - (a & ~1)) & 0xFFFFFFFF))
    return out


def glitch_filter(segs, min_us):
    out = []
    for lvl, d in segs:
        if out and (d < min_us or out[-1][0] == lvl):
            out[-1] = (out[-1][0], out[-1][1] + d)      # absorb short / same-level
        else:
            out.append((lvl, d))
    # second pass: coalesce neighbours that ended up with equal level
    res = []
    for lvl, d in out:
        if res and res[-1][0] == lvl:
            res[-1] = (lvl, res[-1][1] + d)
        else:
            res.append((lvl, d))
    return res


def analyze(path, min_us):
    raw = open(path, "rb").read()
    segs = segments(raw)
    total = sum(d for _, d in segs) / 1e6
    print(f"{path}: {len(segs)} raw segments over {total:.2f}s ({len(segs)/max(total,1e-9):.0f} edges/s)")
    hist = {}
    for _, d in segs:
        b = 25 if d < 25 else 50 if d < 50 else 75 if d < 75 else 100 if d < 100 else 200 if d < 200 else 500 if d < 500 else 2000 if d < 2000 else 99999
        hist[b] = hist.get(b, 0) + 1
    print("raw duration histogram (<us: count):", dict(sorted(hist.items())))
    f = glitch_filter(segs, min_us)
    print(f"after glitch filter {min_us} us: {len(f)} segments")
    # Receiver output is active-low: level 0 = IR burst (mark).
    marks = [d for l, d in f if l == 0 and d < 20000]
    if marks:
        print(f"marks (<20ms): n={len(marks)} min={min(marks)} max={max(marks)}")
    # Frames = runs of segments separated by idle (level 1) > 20 ms.
    frame, frames = [], []
    for l, d in f:
        if l == 1 and d > 20000:
            if len(frame) > 10:
                frames.append(frame)
            frame = []
        else:
            frame.append((l, d))
    if len(frame) > 10:
        frames.append(frame)
    print(f"candidate frames (>10 segments): {len(frames)}")
    for i, fr in enumerate(frames[:6]):
        seq = " ".join(("M" if l == 0 else "s") + str(d) for l, d in fr[:80])
        print(f" frame {i}: {len(fr)} segs | {seq}{' …' if len(fr) > 80 else ''}")
    return frames


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture"); c.add_argument("--port", default="/dev/cu.usbmodem101")
    c.add_argument("--ms", type=int, default=6000); c.add_argument("--label", default="cap")
    c.add_argument("--say", default=""); c.add_argument("--min-us", type=int, default=150)
    c.add_argument("--no-boost", action="store_true", help="boost off during capture, then reboot")
    a = sub.add_parser("analyze"); a.add_argument("file"); a.add_argument("--min-us", type=int, default=150)
    args = ap.parse_args()
    if args.cmd == "capture":
        extra = {"boost": False, "reboot": True} if args.no_boost else None
        p = capture(args.port, args.ms, args.label, args.say, extra)
        analyze(p, args.min_us)
    else:
        analyze(args.file, args.min_us)
