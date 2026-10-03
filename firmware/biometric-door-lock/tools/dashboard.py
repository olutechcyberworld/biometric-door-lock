#!/usr/bin/env python3
"""Face-lock debugging dashboard.  The browser is the debugging area; the terminal only launches it.

    python dashboard.py --port /dev/ttyACM0          # then open http://localhost:8000
    python dashboard.py --simulate                   # no board needed: demo / self-test

What it shows (all from the board's serial output):
  * live camera view = the exact image the face detector receives (+ boxes, landmarks, exposure stats)
  * firmware / camera / stream status, with the reason when something is wrong (I2C error, sensor not
    detected, clock mismatch, old firmware, no frames, board silent ...) and what to try
  * every boot, whether the camera came up, and a "boot test" that reboots N times and tallies the outcomes
  * the raw serial log (errors highlighted) and buttons: reboot / enroll / recognize / delete / mode / snapshot
Only stdlib + pyserial (pyserial not needed for --simulate).
"""
import argparse, base64, collections, http.server, json, os, random, re, struct, sys, threading, time, zlib
from urllib.parse import parse_qs, urlparse

# =====================================================================================================
# frame protocol + image helpers (same wire format as the firmware's dump_frame)
# =====================================================================================================
class FrameParser:
    def __init__(self):
        self.reset()

    def reset(self):
        self.hdr, self.boxes, self.data = None, [], bytearray()

    def feed(self, line):
        """Returns (kind, payload). kind: 'frame' | 'drop' | None."""
        if line.startswith("FRAME_BEGIN,"):
            p = line.split(",")
            try:
                self.hdr = dict(w=int(p[1]), h=int(p[2]), pix=p[3], nbytes=int(p[4]), step=int(p[5]))
            except (IndexError, ValueError):
                self.hdr = None
                return None, None
            self.boxes, self.data = [], bytearray()
        elif self.hdr is None:
            return None, None
        elif line.startswith("BOX,"):
            try:
                v = [int(x) for x in line.split(",")[1:]]
                self.boxes.append(dict(box=v[0:4], score=v[4] / 1000.0, kp=v[5:]))
            except ValueError:
                pass
        elif line.startswith("D,"):
            try:
                self.data += base64.b64decode(line[2:])
            except Exception:
                self.hdr = None
                return "drop", "bad base64 line inside a frame"
        elif line.startswith("FRAME_END"):
            hdr, boxes, data = self.hdr, self.boxes, bytes(self.data)
            self.reset()
            if len(data) != hdr["nbytes"]:
                return "drop", f"frame corrupt: {len(data)} of {hdr['nbytes']} bytes"
            return "frame", dict(hdr, boxes=boxes, data=data)
        elif line.startswith("FRAME_ERROR"):
            self.reset()
            return "drop", line
        return None, None

    @property
    def in_frame(self):
        return self.hdr is not None


def decode_rgb(f):
    w, h, pix, d = f["w"], f["h"], f["pix"], f["data"]
    n = w * h
    if pix in ("RGB565BE", "RGB565LE"):
        vals = struct.unpack((">" if pix == "RGB565BE" else "<") + "%dH" % n, d)
        rgb = bytearray(n * 3)
        for i, v in enumerate(vals):
            rgb[3 * i] = (((v >> 11) & 31) * 255) // 31
            rgb[3 * i + 1] = (((v >> 5) & 63) * 255) // 63
            rgb[3 * i + 2] = ((v & 31) * 255) // 31
        return rgb
    if pix == "RGB888":
        return bytearray(d)
    if pix == "GRAY":
        return bytearray(b for v in d for b in (v, v, v))
    raise ValueError("unsupported pixel type " + pix)


def luma_stats(rgb):
    ys = [(299 * rgb[i] + 587 * rgb[i + 1] + 114 * rgb[i + 2]) // 1000 for i in range(0, len(rgb), 3)]
    s = sorted(ys)
    n = len(s)
    return dict(mean=round(sum(ys) / n, 1), p5=s[n // 20], p95=s[(19 * n) // 20],
                dark=round(100.0 * sum(1 for y in ys if y < 20) / n, 1),
                bright=round(100.0 * sum(1 for y in ys if y > 235) / n, 1))


def exposure_notes(st):
    notes = []
    if st["mean"] < 45 or st["dark"] > 50:
        notes.append("too dark")
    if st["mean"] > 200 or st["bright"] > 40:
        notes.append("overexposed")
    if st["p95"] - st["p5"] < 35:
        notes.append("flat / low contrast (or camera not really streaming)")
    return notes


def _put(img, W, H, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        i = 3 * (y * W + x)
        img[i:i + 3] = bytes(c)


def render(f, target=720):
    w, h, step = f["w"], f["h"], f["step"]
    rgb = decode_rgb(f)
    st = luma_stats(rgb)
    up = max(1, target // w)
    W, H = w * up, h * up
    img = bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += rgb[3 * (y * w + x):3 * (y * w + x) + 3] * up
        img += row * up
    s = up / step
    for b in f["boxes"]:
        x1, y1, x2, y2 = [int(v * s) for v in b["box"]]
        for k in range(2):
            for x in range(x1, x2 + 1):
                _put(img, W, H, x, y1 + k, (0, 255, 0)); _put(img, W, H, x, y2 - k, (0, 255, 0))
            for y in range(y1, y2 + 1):
                _put(img, W, H, x1 + k, y, (0, 255, 0)); _put(img, W, H, x2 - k, y, (0, 255, 0))
        kp = b["kp"]
        for i in range(0, len(kp) - 1, 2):
            cx, cy = int(kp[i] * s), int(kp[i + 1] * s)
            for dx in range(-3, 4):
                for dy in range(-3, 4):
                    _put(img, W, H, cx + dx, cy + dy, (255, 0, 0))
    for dx in range(14):  # yellow square = top-left corner of what the model receives
        for dy in range(14):
            _put(img, W, H, dx, dy, (255, 255, 0))
    return img, W, H, st


def png_bytes(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))


# =====================================================================================================
# log understanding
# =====================================================================================================
RE_IDF = re.compile(r"^([EWID]) \((\d+)\) ([^:]+): (.*)$")
CAM_TAGS = {"ov3660", "ov2640", "sccb_i2c", "i2c.master", "esp_video_init", "esp_video", "dvp_video",
            "V4L2Device", "VideoCapture", "dvp_ext", "esp_cam_sensor"}

def classify_cam_error(msg):
    if "Camera sensor is not" in msg or "failed to detect" in msg or "get sensor ID failed" in msg:
        return ("sensor not detected",
                "The sensor did not answer on the camera bus (SCCB/I2C). If it only happens some boots: bus timing, "
                "power, wiring/connector, or the camera not being power-cycled by a soft reset. Run the boot test.")
    if "I2C bus is still busy" in msg or "failed to i2c transmit" in msg or "Set common regs failed" in msg \
            or "failed to set basic format" in msg:
        return ("camera bus error while configuring the sensor",
                "An I2C/SCCB transaction failed mid-configuration. Same family as above: intermittent = signal "
                "integrity, power sag, or connector. Run the boot test to measure how often.")
    if "Failed to open device" in msg or "Device not initialized" in msg or "video->ops->init" in msg:
        return ("video device did not initialise", "Follow-on from an earlier camera error in the same boot.")
    return ("camera error: " + msg[:90], "See the log below.")


class Dashboard:
    def __init__(self, ser, baud, mode, outdir):
        self.ser, self.baud, self.outdir = ser, baud, outdir
        self.lock = threading.RLock()
        self.wlock = threading.Lock()
        self.t_start = time.time()
        self.log = collections.deque(maxlen=400)
        self.rx_bytes, self.last_rx = 0, 0.0
        self.parser = FrameParser()
        self.boots = []            # list of dict
        self.fwinfo = None         # dict from FWINFO line
        self.console_ready = False
        self.hb = collections.deque(maxlen=6)   # (t_pc, uptime_ms, frames, detect_us)
        self.frame_png, self.frame_n, self.frame_t, self.frame_meta = None, 0, 0.0, None
        self.frame_seq = 0
        self.dump_timeouts = 0
        self.stream_on, self.mode = True, mode
        self.bench_last = None
        self.enroll_msgs = collections.deque(maxlen=5)
        self.boottest = dict(running=False, total=0, done=0, ok=0, fail=0, incomplete=0, reasons={}, msg="")
        self.reboot_msg = ""
        self.drops = collections.deque(maxlen=400)   # (t_pc, expected_bytes, received_bytes) from the DVP driver
        self.drops_total = 0
        self.crashes = collections.deque(maxlen=10)  # (t_pc, kind)
        # fingerprint (AS608) state, fed by FPINIT / FPSTATE / FP,... lines from the firmware
        self.fp = dict(init=None, capacity=None, count=None, security=None, baud=None, reason="", code=None)
        self.fp_prompt, self.fp_prompt_t = "", 0.0
        self.fp_hist = collections.deque(maxlen=40)
        self.fp_stats = dict(match=0, nomatch=0, timeout=0, error=0, proc=[], wait=[])
        self.stop = False
        os.makedirs(outdir, exist_ok=True)

    # ------------------------------------------------------------------ serial out
    def send(self, chars):
        with self.wlock:
            try:
                self.ser.write(chars.encode())
                return True
            except Exception as e:  # noqa
                self._log("err", f"[dashboard] serial write failed: {e}")
                return False

    def reboot(self):
        """Reset via RTS (EN) like esptool does. Only works if the board's USB-UART has the auto-reset circuit."""
        n0 = len(self.boots)
        try:
            self.ser.dtr = False
            self.ser.rts = True
            time.sleep(0.12)
            self.ser.rts = False
        except Exception as e:  # noqa
            self.reboot_msg = f"RTS reset not available on this port ({e}). Press the board's RESET button."
            return False
        self.reboot_msg = "reset pulse sent"
        threading.Thread(target=self._reboot_watch, args=(n0,), daemon=True).start()
        return True

    def _reboot_watch(self, n0):
        t_end = time.time() + 6
        while time.time() < t_end:
            if len(self.boots) > n0:
                self.reboot_msg = "board rebooted"
                return
            time.sleep(0.1)
        self.reboot_msg = "no reboot seen after RTS pulse - press the RESET button (auto-reset may not be wired)"

    # ------------------------------------------------------------------ serial in
    def _log(self, lvl, text):
        with self.lock:
            if self.log and self.log[-1]["text"] == text and self.log[-1]["lvl"] == lvl:
                self.log[-1]["n"] += 1
            else:
                self.log.append(dict(t=time.strftime("%H:%M:%S.") + f"{int(time.time()*1000)%1000:03d}", lvl=lvl, text=text, n=1))

    def reader(self):
        buf = b""
        while not self.stop:
            try:
                chunk = self.ser.readline()
            except Exception as e:  # noqa
                self._log("err", f"[dashboard] serial read failed: {e}")
                time.sleep(1)
                continue
            if not chunk:
                continue
            self.rx_bytes += len(chunk)
            self.last_rx = time.time()
            self.on_line(chunk.decode(errors="replace").rstrip("\r\n"))

    def _cur_boot(self):
        return self.boots[-1] if self.boots else None

    @staticmethod
    def _errs(b):
        """Camera errors that still matter: none if a later init attempt in the same boot succeeded."""
        return [] if b.get("cam_ok") else b["errors"]

    def _new_boot(self, why):
        b = dict(n=len(self.boots) + 1, t=time.strftime("%H:%M:%S"), t0=time.time(), detected=False, pid=None,
                 driver_init=False, ready=False, ready_t=None, errors=[], warnings=[], why=why,
                 attempts=0, cam_ok=None)
        self.boots.append(b)
        self.console_ready, self.fwinfo = False, None
        self.hb.clear()
        return b

    def on_line(self, line):
        with self.lock:
            # ---- frame protocol first (never goes to the log); other lines interleaved in a dump still get logged
            if line.startswith(("FRAME_BEGIN,", "FRAME_END", "FRAME_ERROR", "BOX,", "D,")):
                kind, payload = self.parser.feed(line)
                if kind == "frame":
                    self._on_frame(payload)
                elif kind == "drop":
                    self._log("warn", "[dashboard] " + payload)
                return
            # ---- camera driver frame-size errors: count them, don't flood the log
            if line.startswith("E:RX:"):
                try:
                    exp, got = line[5:].split("-")
                    self.drops.append((time.time(), int(exp), int(got)))
                    self.drops_total += 1
                except ValueError:
                    self._log("raw", line)
                return
            if self.parser.in_frame and not RE_IDF.match(line) and not line.startswith(("HB,", "BENCH,", "FWINFO,")):
                return  # stray fragment of a frame line that got cut by another writer; the frame is dropped anyway
            # ---- heartbeat / info
            if line.startswith("HB,"):
                try:
                    p = line.split(",")
                    cam = int(p[4]) if len(p) > 4 else None   # frames the camera delivered (newer firmware)
                    self.hb.append((time.time(), int(p[1]), int(p[2]), int(p[3]), cam))
                except (ValueError, IndexError):
                    pass
                return
            if line.startswith("CAMINIT,"):
                try:
                    kv = dict(x.split("=", 1) for x in line.split(",")[1:])
                    b0 = self._cur_boot() or self._new_boot("attached")
                    b0["attempts"] = int(kv.get("attempt", 0))
                    b0["cam_ok"] = kv.get("result") == "ok"
                except (ValueError, KeyError):
                    pass
                self._log("info", line)
                return
            if line.startswith(("FPINIT,", "FPSTATE,", "FP,")):
                self._on_fp(line)
                return
            if line.startswith("FWINFO,"):
                self.fwinfo = dict(kv.split("=", 1) for kv in line.split(",")[1:] if "=" in kv)
                self._log("info", line)
                return
            if line.startswith("BENCH,"):
                try:
                    p = line.split(",")
                    self.bench_last = dict(faces=int(p[2]), detect_ms=int(p[3]) / 1000, recog_ms=int(p[4]) / 1000,
                                           e2e_ms=int(p[5]) / 1000, id=int(p[6]), sim=float(p[7]),
                                           t=time.strftime("%H:%M:%S"))
                except (ValueError, IndexError):
                    pass
                self._log("info", line)
                return
            # ---- normal log line
            m = RE_IDF.match(line)
            lvl, tag, msg = ("info", "", line)
            if m:
                lvl = {"E": "err", "W": "warn", "I": "info", "D": "info"}[m.group(1)]
                tag, msg = m.group(3), m.group(4)
            elif line.startswith("E:"):
                lvl = "raw"
            self._log(lvl, line)
            if tag == "task_wdt" and "did not reset the watchdog" in msg:
                self.crashes.append((time.time(), "wdt"))
            if "Guru Meditation" in line or "abort() was called" in line or line.startswith("Rebooting..."):
                self.crashes.append((time.time(), "panic"))
            if "boot: ESP-IDF v" in line or line.startswith("ESP-ROM:"):
                cb = self._cur_boot()
                if not (cb and cb["why"] == "marker" and time.time() - cb["t0"] < 1.0):
                    self._new_boot("marker")
                return
            if "Calling app_main" in line and not self._cur_boot():
                self._new_boot("attached")
            b = self._cur_boot()
            if b is None and (tag in CAM_TAGS or "console ready" in line):
                b = self._new_boot("attached")
            if b is None:
                return
            if "Detected Camera sensor" in msg:   # any sensor driver: ov2640, ov3660, ...
                b["detected"] = True
                mm = re.search(r"PID=(0x[0-9a-fA-F]+)", msg)
                b["pid"] = mm.group(1) if mm else None
            if tag == "dvp_ext" and "initialized" in msg:
                b["driver_init"] = True
            if tag == "esp_video_init" and "xclk frequency" in msg:
                b["warnings"].append("camera clock mismatch")
            if lvl == "err" and tag in CAM_TAGS:
                reason = classify_cam_error(msg)
                if reason[0] not in [e[0] for e in b["errors"]]:
                    b["errors"].append(reason)
            if "console ready" in line:
                b["ready"], b["ready_t"], self.console_ready = True, time.time(), True
            if tag == "bench_app" or "HumanFaceRecognizer" in tag:
                self.enroll_msgs.append(f"{time.strftime('%H:%M:%S')}  {msg}")

    @staticmethod
    def _kv(fields):
        out = {}
        for x in fields:
            if "=" in x:
                k, v = x.split("=", 1)
                out[k] = v
        return out

    def _on_fp(self, line):
        parts = line.split(",")
        now = time.strftime("%H:%M:%S")
        if parts[0] == "FPINIT":
            kv = self._kv(parts[1:])
            if kv.get("result") == "ok":
                self.fp.update(init="ok", capacity=int(kv.get("capacity", 0)), count=int(kv.get("count", 0)),
                               security=kv.get("security"), baud=kv.get("baud"), reason="", code=None)
            else:
                self.fp.update(init="fail", reason=kv.get("reason", "no response"), code=kv.get("code"))
            self._log("info" if self.fp["init"] == "ok" else "err", line)
            return
        if parts[0] == "FPSTATE":
            kv = self._kv(parts[1:])
            try:
                self.fp["count"], self.fp["capacity"] = int(kv["count"]), int(kv["capacity"])
            except (KeyError, ValueError):
                pass
            return
        kind = parts[1] if len(parts) > 1 else ""
        kv = self._kv(parts[2:])
        prompts = {"place": "Place a finger on the sensor", "start": "Enrolling into slot " + kv.get("id", "?"),
                   "place1": "Place your finger on the sensor", "remove": "Lift your finger",
                   "place2": "Place the SAME finger again", "storing": "Saving the fingerprint..."}
        if "step" in kv:
            self.fp_prompt, self.fp_prompt_t = prompts.get(kv["step"], kv["step"]), time.time()
            return
        if "result" in kv:
            self.fp_prompt = ""
            res = kv["result"]
            if res == "confirm":
                self.fp_prompt, self.fp_prompt_t = "Erase confirmation pending", time.time()
            entry = dict(t=now, kind=kind, result=res, id=kv.get("id"), score=kv.get("score"), reason=kv.get("reason", ""),
                         wait=kv.get("wait_ms"), capture=kv.get("capture_ms"), search=kv.get("search_ms"), ms=kv.get("ms"))
            self.fp_hist.append(entry)
            if kind == "identify" and res in self.fp_stats:
                self.fp_stats[res] += 1
                if kv.get("capture_ms") and kv.get("search_ms"):
                    self.fp_stats["proc"].append(int(kv["capture_ms"]) + int(kv["search_ms"]))
                if kv.get("wait_ms"):
                    self.fp_stats["wait"].append(int(kv["wait_ms"]))
            self._log("info" if res in ("match", "ok", "nomatch", "confirm") else "warn", line)

    def _on_frame(self, f):
        try:
            img, W, H, st = render(f)
        except Exception as e:  # noqa
            self._log("err", f"[dashboard] cannot render frame: {e}")
            return
        self.frame_png = png_bytes(W, H, img)
        self.frame_n += 1
        self.frame_seq += 1
        self.frame_t = time.time()
        self.dump_timeouts = 0
        self.frame_meta = dict(w=f["w"], h=f["h"], pix=f["pix"], step=f["step"], stats=st,
                               faces=len(f["boxes"]), score=(f["boxes"][0]["score"] if f["boxes"] else None),
                               notes=exposure_notes(st))

    # ------------------------------------------------------------------ streaming
    def _dump_timeout(self):
        est = {"q": 60 * 60 * 2, "h": 120 * 120 * 2, "v": 240 * 240 * 2}[self.mode] * 1.36 * 10 / self.baud
        return est * 1.5 + 4

    def streamer(self):
        while not self.stop:
            if not self.stream_on or self.boottest["running"]:
                time.sleep(0.2)
                continue
            seq0 = self.frame_seq
            self.send(self.mode)
            t_end = time.time() + self._dump_timeout()
            while time.time() < t_end and self.frame_seq == seq0 and self.stream_on and not self.stop:
                time.sleep(0.02)
            if self.frame_seq == seq0:
                self.dump_timeouts += 1
                time.sleep(1.0)
            time.sleep(0.05)

    # ------------------------------------------------------------------ boot test
    def run_boot_test(self, n):
        bt = self.boottest
        bt.update(running=True, total=n, done=0, ok=0, fail=0, incomplete=0, reasons={}, msg="running")
        for _ in range(n):
            if not bt["running"]:
                break
            n0 = len(self.boots)
            if not self.reboot():
                bt.update(running=False, msg=self.reboot_msg)
                return
            t_end = time.time() + 22
            while time.time() < t_end:
                if len(self.boots) > n0:
                    b = self.boots[-1]
                    if b["ready_t"] and time.time() - b["ready_t"] > 2.0:
                        break
                time.sleep(0.1)
            if len(self.boots) <= n0:
                bt["incomplete"] += 1
                bt["reasons"]["no reboot seen"] = bt["reasons"].get("no reboot seen", 0) + 1
            else:
                b = self.boots[-1]
                if self._errs(b):
                    bt["fail"] += 1
                    bt["reasons"][self._errs(b)[0][0]] = bt["reasons"].get(self._errs(b)[0][0], 0) + 1
                elif b.get("cam_ok") or (b["detected"] and b["driver_init"]):
                    bt["ok"] += 1
                    if b.get("attempts", 0) > 1:
                        bt["reasons"]["OK but needed retries"] = bt["reasons"].get("OK but needed retries", 0) + 1
                        bt["reasons"]["extra restarts by firmware"] = (bt["reasons"].get("extra restarts by firmware", 0)
                                                                      + b["attempts"] - 1)
                else:
                    bt["incomplete"] += 1
                    bt["reasons"]["camera init never completed"] = bt["reasons"].get("camera init never completed", 0) + 1
            bt["done"] += 1
            time.sleep(0.5)
        bt.update(running=False, msg="finished")

    # ------------------------------------------------------------------ diagnosis
    def _fps(self, idx=2):
        """Frames per second from the heartbeat counters. idx 2 = frames seen by the detector, 4 = by the camera."""
        if len(self.hb) < 2 or self.hb[0][idx] is None or self.hb[-1][idx] is None:
            return None
        a, b = self.hb[0], self.hb[-1]
        dt = (b[1] - a[1]) / 1000.0
        return round((b[idx] - a[idx]) / dt, 1) if dt > 0 else None

    def fw_has_fp(self):
        return bool(self.fwinfo and self.fwinfo.get("fp") == "1")

    def _drop_rate(self, window=5.0):
        now = time.time()
        recent = [d for d in self.drops if now - d[0] <= window]
        if not recent:
            return 0.0, None
        top = collections.Counter((d[1], d[2]) for d in recent).most_common(1)[0][0]
        return round(len(recent) / window, 1), top

    def diagnose(self):
        P, now = [], time.time()
        b = self._cur_boot()
        rate, top = self._drop_rate()
        if rate >= 2:
            P.append(("error", f"Camera driver is dropping incomplete frames: {rate}/s (received {top[1]} of {top[0]} bytes)",
                      "Every frame must be exactly %d bytes; the driver discards any other size. Causes to check in order: "
                      "(1) camera clock still 16 MHz - apply/flash phase1_camera_fix.patch; (2) unstable sync/pixel-clock "
                      "signals (connector seating, wiring, power); (3) CPU/PSRAM contention. After fixing, this rate should "
                      "fall towards 0." % top[0]))
        for t, kind in list(self.crashes):
            if now - t < 90:
                if kind == "wdt":
                    P.append(("error", "Task watchdog fired: something hogged CPU 1 for more than 5 s",
                              "Usually a long frame dump at low baud (full-res at 115200 is ~14 s per frame). Flash "
                              "phase1_wdt_fix.patch, use medium mode, or raise the baud rate."))
                else:
                    P.append(("error", "Board crashed / rebooted", "See the log around the backtrace."))
                break
        if self.rx_bytes == 0 and now - self.t_start > 4:
            P.append(("error", "No data from the board at all",
                      "Wrong port, board not running, or you attached mid-run. Press Reboot (or the board's RESET "
                      "button). Check which USB-C port you are on (the UART one)."))
        elif self.rx_bytes and now - self.last_rx > 6:
            P.append(("warn", f"Board silent for {int(now - self.last_rx)} s",
                      "Crashed, unplugged, or stuck. Press Reboot."))
        if b:
            for name, hint in self._errs(b):
                P.append(("error", f"Camera: {name}", hint))
            if b.get("cam_ok") and b.get("attempts", 0) > 1:
                P.append(("warn", f"Camera needed {b['attempts']} init attempts this boot (recovered by retry)",
                          "The sensor-ID read is intermittently wrong on this board. Retries hide it; the boot test "
                          "shows how often it happens. If most boots need retries, suspect bus signal integrity."))
            if "camera clock mismatch" in b["warnings"]:
                P.append(("info", "Camera clock differs from the sensor mode's nominal input (esp_video warning)",
                          "The stock S3-EYE BSP drives the sensor at 16 MHz while the sensor mode tables assume 20 MHz. "
                          "Usually harmless (slightly lower frame rate). It matters only if frames are dropped or missing: "
                          "check the 'Dropped frames' and 'Camera frames' rows."))
        if self.console_ready and self.fwinfo is None:
            P.append(("warn", "Firmware has no frame-dump support",
                      "This is an older build. Apply the viewer patches, rebuild and flash; until then no image can "
                      "be shown."))
        fps = self._fps()
        cfps = self._fps(4)
        if self.fp["init"] == "fail":
            P.append(("error", "Fingerprint sensor not responding: " + (self.fp["reason"] or "no reason given"),
                      "Check in order: (1) module TX goes to the ESP RX GPIO and module RX to the ESP TX GPIO (crossed); "
                      "(2) module powered from 5 V with a common ground; (3) module baud is 57600; (4) the GPIOs in "
                      "menuconfig (AS608 fingerprint sensor) match your wiring. Press Probe after fixing."))
        elif self.fw_has_fp() and self.fp["init"] is None and self.console_ready and b and time.time() - (b.get("ready_t") or 0) > 8:
            P.append(("warn", "No fingerprint init message seen since boot", "Press Probe, or reboot the board."))
        if self.console_ready and len(self.hb) >= 3 and not (b and self._errs(b)):
            if cfps is not None and cfps < 0.5:
                P.append(("error", "The camera delivers no frames at all (nothing reaches the pipeline)",
                          "Sensor was detected and initialised, but no frame ever completes. Suspects: camera clock "
                          "(20 MHz vs 16 MHz), sensor not streaming, or sync/pixel-clock lines. No 'E:RX' drops "
                          "means the driver never even saw a frame end (no VSYNC)."))
            elif cfps is not None and cfps >= 0.5 and fps is not None and fps < 0.2 and not self.stream_on:
                P.append(("error", "Frames reach the pipeline but the detector is not consuming them",
                          "Detector task stuck or crashed: check the log for backtraces."))
            elif cfps is None and fps is not None and fps < 0.5:
                P.append(("error", "Firmware is up but the detector is receiving no camera frames",
                          "Camera streaming did not start (check the camera lines in the log)."))
        if self.fwinfo and self.dump_timeouts >= 2 and not (b and self._errs(b)) and (fps or 0) >= 0.5:
            P.append(("error", "Camera is streaming but frame dumps are not arriving",
                      "Serial dump blocked or corrupted. Check the log for 'frame corrupt'; try Mode: Live."))
        if self.frame_meta and now - self.frame_t < 30:
            fm = self.frame_meta
            for n in fm["notes"]:
                P.append(("warn", f"Image: {n}", "Check lighting / exposure. OV3660 auto-exposure needs a second or "
                                                  "two after start."))
            if fm["faces"] == 0 and not fm["notes"]:
                P.append(("info", "Image looks fine but no face detected",
                          "Check face size (fill 30-70% of the height), distance ~30-50 cm, and that the face is "
                          "upright: the yellow square marks the top-left of what the model sees."))
        if not P:
            P.append(("ok", "No problems detected", ""))
        return P

    def state(self):
        with self.lock:
            b = self._cur_boot()
            fps = self._fps()
            drate, dtop = self._drop_rate()
            if b and self._errs(b):
                cam = dict(status="FAILED", detail=self._errs(b)[0][0])
            elif drate >= 2:
                cam = dict(status="DEGRADED", detail=f"{drate} incomplete frames/s dropped by the camera driver")
            elif self.console_ready and len(self.hb) >= 3 and self._fps(4) is not None and self._fps(4) < 0.5:
                cam = dict(status="NO FRAMES", detail="sensor initialised but the camera delivers 0 frames/s")
            elif fps and fps >= 0.5:
                cam = dict(status="STREAMING", detail=f"{fps} fps into the detector")
            elif b and b["detected"] and b["driver_init"]:
                cam = dict(status="OK", detail=f"sensor PID {b['pid']} initialised")
            elif b:
                cam = dict(status="INITIALISING", detail="waiting for camera init messages")
            else:
                cam = dict(status="UNKNOWN", detail="no boot seen yet (press Reboot)")
            fm = self.frame_meta
            return dict(
                now=time.time(), rx_bytes=self.rx_bytes, last_rx_age=(time.time() - self.last_rx) if self.last_rx else None,
                baud=self.baud, mode=self.mode, stream_on=self.stream_on,
                fw=dict(ready=self.console_ready, info=self.fwinfo,
                        uptime_s=(self.hb[-1][1] / 1000.0 if self.hb else None),
                        frames=(self.hb[-1][2] if self.hb else None), fps=fps,
                        cam_frames=(self.hb[-1][4] if self.hb else None), cam_fps=self._fps(4),
                        detect_ms=(self.hb[-1][3] / 1000.0 if self.hb else None)),
                cam=cam,
                drops=dict(total=self.drops_total, rate=self._drop_rate()[0],
                           last=(list(self.drops[-1][1:]) if self.drops else None)),
                frame=dict(n=self.frame_n, age=(time.time() - self.frame_t) if self.frame_t else None, meta=fm),
                bench=self.bench_last, enroll=list(self.enroll_msgs),
                boots=[dict(n=x["n"], t=x["t"], detected=x["detected"], pid=x["pid"], driver_init=x["driver_init"],
                            errors=[e[0] for e in self._errs(x)], warnings=x["warnings"],
                            attempts=x.get("attempts", 0))
                       for x in self.boots[-12:]],
                fp=self._fp_state(),
                boottest=self.boottest, reboot_msg=self.reboot_msg,
                problems=[dict(sev=s, title=t, hint=h) for s, t, h in self.diagnose()],
                log=list(self.log)[-160:],
            )

    def _fp_state(self):
        st = self.fp_stats
        mean = lambda v: round(sum(v) / len(v)) if v else None
        prompt = self.fp_prompt if time.time() - self.fp_prompt_t < 30 else ""
        return dict(supported=self.fw_has_fp(), **self.fp, prompt=prompt, hist=list(self.fp_hist)[-10:][::-1],
                    stats=dict(match=st["match"], nomatch=st["nomatch"], timeout=st["timeout"], error=st["error"],
                               proc_ms=mean(st["proc"]), wait_ms=mean(st["wait"])))

    def snapshot(self):
        with self.lock:
            if not self.frame_png:
                return None
            m = self.frame_meta
            name = f"snap_{time.strftime('%Y%m%d_%H%M%S')}_mean{m['stats']['mean']:.0f}_faces{m['faces']}.png"
            path = os.path.join(self.outdir, name)
            with open(path, "wb") as fh:
                fh.write(self.frame_png)
            return path


# =====================================================================================================
# web UI
# =====================================================================================================
PAGE = r"""<!doctype html><html><head><meta charset=utf-8><title>face-lock debug</title>
<style>
:root{--bg:#0f1115;--card:#171a21;--line:#262b36;--tx:#d7dbe4;--dim:#8a92a3;--ok:#3ecf72;--warn:#e8b430;--err:#ef5b5b;--info:#5aa7ff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:13px/1.45 ui-monospace,Menlo,Consolas,monospace}
header{display:flex;gap:10px;align-items:center;padding:10px 14px;border-bottom:1px solid var(--line);flex-wrap:wrap}
h1{font-size:15px;margin:0 12px 0 0}.pill{padding:2px 9px;border-radius:99px;background:#222835;color:var(--dim)}
.pill.ok{background:#173625;color:var(--ok)}.pill.warn{background:#3a2f12;color:var(--warn)}.pill.err{background:#3b1a1a;color:var(--err)}
#banner{padding:0 14px}.prob{margin:8px 0;padding:8px 12px;border-radius:6px;border-left:4px solid var(--info);background:var(--card)}
.prob.error{border-color:var(--err)}.prob.warn{border-color:var(--warn)}.prob.ok{border-color:var(--ok)}.prob b{display:block}.prob span{color:var(--dim)}
main{display:grid;grid-template-columns:minmax(300px,1.1fr) minmax(300px,1fr);gap:12px;padding:0 14px 14px}
@media(max-width:900px){main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 12px;margin-bottom:12px}
.card h2{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin:0 0 8px}
#view{width:100%;aspect-ratio:1/1;background:#000;display:flex;align-items:center;justify-content:center;border-radius:6px;overflow:hidden;position:relative}
#view img{width:100%;height:100%;object-fit:contain;image-rendering:pixelated}#ph{position:absolute;padding:20px;text-align:center;color:var(--dim)}
.kv{display:grid;grid-template-columns:130px 1fr;gap:2px 8px}.kv div:nth-child(odd){color:var(--dim)}
.btns{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}button{background:#222835;color:var(--tx);border:1px solid var(--line);border-radius:6px;padding:6px 10px;font:inherit;cursor:pointer}
button:hover{background:#2b3242}button.on{border-color:var(--info);color:var(--info)}
table{border-collapse:collapse;width:100%}td,th{padding:2px 6px;border-bottom:1px solid var(--line);text-align:left;font-weight:normal}th{color:var(--dim)}
#enr{white-space:pre-wrap}#log{height:260px;overflow:auto;background:#0b0d11;border-radius:6px;padding:6px 8px;white-space:pre-wrap;word-break:break-all}
.l-err{color:var(--err)}.l-warn{color:var(--warn)}.l-raw{color:#6b7385}.l-info{color:#aab3c5}.rep{color:var(--dim)}
</style></head><body>
<header><h1>face-lock debug</h1><span id=p_ser class=pill>serial</span><span id=p_fw class=pill>firmware</span>
<span id=p_cam class=pill>camera</span><span id=p_str class=pill>stream</span><span id=p_msg class=pill></span></header>
<div id=banner></div>
<main><div>
 <div class=card><h2>What the model sees</h2>
  <div id=view><img id=img alt=""><div id=ph>waiting for the first frame...</div></div>
  <div id=fstat class=rep style="margin-top:6px"></div>
  <div class=btns>
   <button id=b_live onclick="cmd('mode','q')">Mode: live (60px)</button>
   <button id=b_half onclick="cmd('mode','h')">Mode: medium (120px)</button>
   <button id=b_full onclick="cmd('mode','v')">Mode: full (240px)</button>
   <button id=b_stream onclick="cmd('stream','toggle')">Pause stream</button>
   <button onclick="cmd('snapshot')">Save snapshot</button>
  </div>
  <div id=modehint class=rep style="margin-top:6px"></div>
  <div class=rep style="margin-top:6px">yellow square = top-left of the image the model receives &middot; green = face &middot; red = landmarks</div>
 </div>
 <div class=card><h2>Log (raw serial)</h2><div id=log></div>
  <div class=btns><button onclick="cmd('clearlog')">Clear</button></div></div>
</div><div>
 <div class=card><h2>Controls</h2><div class=btns style="margin-top:0">
  <button onclick="cmd('reboot')">Reboot board</button><button onclick="cmd('enroll')">Enroll face</button>
  <button onclick="cmd('recognize')">Recognize</button><button onclick="cmd('delete')">Delete last face</button>
  <button onclick="cmd('boottest','10')">Boot test x10</button><button onclick="cmd('boottest','stop')">Stop test</button></div>
  <div id=ctlmsg class=rep style="margin-top:6px"></div></div>
 <div class=card id=fpcard><h2>Fingerprint (AS608)</h2>
  <div id=fpprompt style="display:none;padding:10px 12px;margin-bottom:8px;border-radius:6px;background:#3a2f12;color:#e8b430;font-size:15px"></div>
  <div class=kv id=fpkv></div>
  <div class=btns><button onclick="cmd('fp_identify')">Identify</button><button onclick="cmd('fp_enroll')">Enroll new</button>
   <button onclick="cmd('fp_probe')">Probe sensor</button><button onclick="if(confirm('Erase EVERY stored fingerprint?'))cmd('fp_empty')">Erase all</button></div>
  <table id=fphist style="margin-top:8px"></table></div>
 <div class=card><h2>Status</h2><div class=kv id=kv></div></div>
 <div class=card><h2>Boots (does the camera come up?)</h2><div id=bt class=rep></div><table id=boots></table></div>
 <div class=card><h2>Last recognition / enroll</h2><div class=kv id=rec></div><div id=enr class=rep style="margin-top:6px"></div></div>
</div></main>
<script>
const $=id=>document.getElementById(id);let lastN=-1,lastLogLen=0;
async function cmd(c,v){const r=await fetch('/api/cmd?c='+c+(v?'&v='+v:''),{method:'POST'});const t=await r.text();$('ctlmsg').textContent=t;}
function pill(id,txt,cls){const e=$(id);e.textContent=txt;e.className='pill '+(cls||'')}
function esc(s){return s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}
function kv(el,rows){el.innerHTML=rows.map(r=>'<div>'+r[0]+'</div><div>'+r[1]+'</div>').join('')}
async function tick(){
 let s;try{s=await (await fetch('/api/state')).json()}catch(e){pill('p_ser','dashboard server unreachable','err');return}
 const age=s.last_rx_age;
 pill('p_ser',s.rx_bytes===0?'serial: no data':(age>6?'serial: silent '+Math.round(age)+'s':'serial: '+s.baud+' baud, receiving'),s.rx_bytes===0||age>6?'err':'ok');
 pill('p_fw',s.fw.ready?(s.fw.info?'firmware: viewer build':'firmware: OLD build'):'firmware: not seen',s.fw.ready?(s.fw.info?'ok':'warn'):'');
 pill('p_cam','camera: '+s.cam.status.toLowerCase(),({FAILED:'err','NO FRAMES':'err',DEGRADED:'warn',STREAMING:'ok',OK:'ok',INITIALISING:'warn'})[s.cam.status]||'');
 pill('p_str',s.stream_on?'stream: on ('+s.mode+')':'stream: paused',s.stream_on?'ok':'');
 pill('p_msg',s.reboot_msg||'');$('p_msg').style.display=s.reboot_msg?'':'none';
 $('banner').innerHTML=s.problems.map(p=>'<div class="prob '+p.sev+'"><b>'+esc(p.title)+'</b>'+(p.hint?'<span>'+esc(p.hint)+'</span>':'')+'</div>').join('');
 if(s.frame.n!==lastN&&s.frame.n>0){lastN=s.frame.n;$('img').src='/frame.png?n='+s.frame.n;$('ph').style.display='none'}
 if(s.frame.n===0){$('ph').style.display='';$('ph').textContent=s.cam.status==='FAILED'?'No image: camera failed to start ('+s.cam.detail+')':(s.fw.ready&&!s.fw.info?'No image: firmware has no frame-dump support (old build)':'waiting for the first frame...')}
 const m=s.frame.meta;$('fstat').textContent=m?('frame #'+s.frame.n+' ('+Math.round(s.frame.age)+'s ago) '+m.w+'x'+m.h+' '+m.pix+' | luma mean '+m.stats.mean+' (p5 '+m.stats.p5+', p95 '+m.stats.p95+') dark '+m.stats.dark+'% bright '+m.stats.bright+'% | faces '+m.faces+(m.score!=null?' score '+m.score.toFixed(2):'')):'';
 ['live:q','half:h','full:v'].forEach(x=>{const [n,k]=x.split(':');$('b_'+n).className=s.mode===k?'on':''});
 const est=b=>(b*1.36*10/s.baud).toFixed(1);$('modehint').textContent='time per frame at '+s.baud+' baud: live '+est(7200)+' s, medium '+est(28800)+' s, full '+est(115200)+' s. The model always analyses the full 240x240 frame; these modes only change the preview size.';$('b_stream').textContent=s.stream_on?'Pause stream':'Resume stream';
 kv($('kv'),[['Camera',s.cam.status+' - '+s.cam.detail],['Firmware',s.fw.ready?(s.fw.info?'viewer build (v'+s.fw.info.viewer+')':'old build, no frame dump'):'not seen yet'],
  ['Uptime',s.fw.uptime_s!=null?s.fw.uptime_s.toFixed(0)+' s':'-'],['Camera frames',s.fw.cam_frames!=null?s.fw.cam_frames+' ('+(s.fw.cam_fps??'-')+' fps)':'n/a (older firmware)'],['Frames to detector',s.fw.frames!=null?s.fw.frames+' ('+(s.fw.fps??'-')+' fps)':'-'],
  ['Detect time',s.fw.detect_ms!=null?s.fw.detect_ms.toFixed(1)+' ms':'-'],['Dropped frames',s.drops.total+' total, '+s.drops.rate+'/s'+(s.drops.last?' (last: '+s.drops.last[1]+' of '+s.drops.last[0]+' bytes)':'')],['Serial bytes',s.rx_bytes]]);
 const f=s.fp;
 if(!f.supported){$('fpcard').style.display=s.fw.ready?'none':'';}else{$('fpcard').style.display='';}
 const fpp=$('fpprompt');fpp.style.display=f.prompt?'':'none';fpp.textContent=f.prompt||'';
 kv($('fpkv'),[['Sensor',f.init==='ok'?'OK':(f.init==='fail'?'NOT RESPONDING - '+esc(f.reason||''):'not probed yet')],
  ['Stored prints',f.count!=null?f.count+' / '+f.capacity:'-'],['Security level',f.security??'-'],['Baud',f.baud??'-'],
  ['Identify results',f.stats.match+' match, '+f.stats.nomatch+' no match, '+f.stats.timeout+' timeout, '+f.stats.error+' error'],
  ['Mean latency',f.stats.proc_ms!=null?f.stats.proc_ms+' ms capture+search (finger wait '+f.stats.wait_ms+' ms)':'-']]);
 $('fphist').innerHTML=f.hist.length?'<tr><th>time</th><th>op</th><th>result</th><th>id</th><th>score</th><th>ms</th></tr>'+f.hist.map(h=>'<tr><td>'+h.t+'</td><td>'+h.kind+'</td><td>'+esc(h.result+(h.reason?' ('+h.reason+')':''))+'</td><td>'+(h.id??'')+'</td><td>'+(h.score??'')+'</td><td>'+(h.capture&&h.search?(+h.capture+ +h.search):(h.ms??''))+'</td></tr>').join(''):'';
 const bs=s.boots.slice().reverse();
 $('boots').innerHTML='<tr><th>#</th><th>time</th><th>sensor</th><th>driver</th><th>result</th></tr>'+bs.map(b=>'<tr><td>'+b.n+'</td><td>'+b.t+'</td><td>'+(b.detected?b.pid:'-')+'</td><td>'+(b.driver_init?'yes':'-')+'</td><td class="'+(b.errors.length?'l-err':(b.detected&&b.driver_init?'':'l-warn'))+'">'+(b.errors.length?esc(b.errors[0]):(b.detected&&b.driver_init?'OK'+(b.attempts>1?' after '+b.attempts+' attempts':'')+(b.warnings.length?' ('+b.warnings[0]+')':''):'incomplete'))+'</td></tr>').join('');
 const t=s.boottest;$('bt').textContent=t.total?('boot test: '+t.done+'/'+t.total+'  OK '+t.ok+'  FAILED '+t.fail+'  incomplete '+t.incomplete+(Object.keys(t.reasons).length?'  ['+Object.entries(t.reasons).map(e=>e[0]+' x'+e[1]).join('; ')+']':'')+(t.running?'  (running...)':'')):'';
 const bn=s.bench;kv($('rec'),bn?[['when',bn.t],['faces',bn.faces],['detect / recog',bn.detect_ms.toFixed(0)+' / '+bn.recog_ms.toFixed(0)+' ms'],['end-to-end',bn.e2e_ms.toFixed(0)+' ms'],['id / similarity',bn.id+' / '+bn.sim.toFixed(3)]]:[['-','no recognition yet']]);
 $('enr').textContent=s.enroll.join('\n');
 const lg=$('log'),stick=lg.scrollTop+lg.clientHeight>=lg.scrollHeight-8;
 lg.innerHTML=s.log.map(l=>'<div class="l-'+l.lvl+'">'+l.t+'  '+esc(l.text)+(l.n>1?' <span class=rep>(x'+l.n+')</span>':'')+'</div>').join('');
 if(stick)lg.scrollTop=lg.scrollHeight;
}
setInterval(tick,600);tick();
</script></body></html>"""


def make_handler(dash):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            if u.path in ("/", "/index.html"):
                self._send(200, PAGE.encode(), "text/html; charset=utf-8")
            elif u.path == "/api/state":
                self._send(200, json.dumps(dash.state()).encode(), "application/json")
            elif u.path == "/frame.png":
                png = dash.frame_png
                if png:
                    self._send(200, png, "image/png")
                else:
                    self._send(204, b"", "text/plain")
            else:
                self._send(404, b"not found", "text/plain")

        def do_POST(self):
            u = urlparse(self.path)
            if u.path != "/api/cmd":
                self._send(404, b"not found", "text/plain")
                return
            q = parse_qs(u.query)
            c, v = q.get("c", [""])[0], q.get("v", [""])[0]
            msg = "ok"
            if c == "reboot":
                msg = "reset pulse sent" if dash.reboot() else dash.reboot_msg
            elif c == "enroll":
                dash.send("e"); msg = "enroll sent - keep your face in frame"
            elif c == "recognize":
                dash.send("r"); msg = "recognize sent (see 'Last recognition')"
            elif c == "delete":
                dash.send("d"); msg = "delete-last sent"
            elif c == "fp_identify":
                dash._log("info", "[button] fp_identify clicked -> sending 'f'"); dash.send("f")
                msg = "identify started - place a finger"
            elif c == "fp_enroll":
                dash._log("info", "[button] fp_enroll clicked -> sending 'n'"); dash.send("n")
                msg = "enrollment started - follow the prompts"
            elif c == "fp_probe":
                dash._log("info", "[button] fp_probe clicked -> sending 'p'"); dash.send("p")
                msg = "probing the fingerprint sensor"
            elif c == "fp_empty":
                dash._log("info", "[button] fp_empty clicked -> sending 'X' x2")
                dash.send("X"); time.sleep(0.5); dash.send("X"); msg = "erase sent"
            elif c == "mode" and v in ("q", "h", "v"):
                dash.mode = v; msg = f"mode {v}"
            elif c == "stream":
                dash.stream_on = not dash.stream_on if v == "toggle" else (v == "on")
                msg = "stream on" if dash.stream_on else "stream paused"
            elif c == "snapshot":
                p = dash.snapshot(); msg = ("saved " + p) if p else "no frame yet"
            elif c == "clearlog":
                dash.log.clear(); msg = "cleared"
            elif c == "boottest":
                if v == "stop":
                    dash.boottest["running"] = False; msg = "stopping"
                elif dash.boottest["running"]:
                    msg = "already running"
                else:
                    threading.Thread(target=dash.run_boot_test, args=(int(v or 10),), daemon=True).start()
                    msg = f"boot test x{v or 10} started (streaming paused meanwhile)"
            else:
                msg = "unknown command"
            self._send(200, msg.encode(), "text/plain")
    return H


# =====================================================================================================
# simulator (no hardware): behaves like the firmware, including flaky camera boots and slow UART
# =====================================================================================================
class FakeSerial:
    def __init__(self, baud, scenario):
        import queue
        self.q, self.baud, self.scenario = queue.Queue(), baud, scenario
        self._rts = False
        self.dtr = False
        self.cmds = queue.Queue()
        self.up = False
        self.frames = 0
        self.face_on = True
        self.prints = []
        threading.Thread(target=self._run, daemon=True).start()
        self.cmds.put("BOOT")

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, v):
        if self._rts and not v:
            self.cmds.put("BOOT")
        self._rts = v

    def write(self, b):
        for ch in b.decode():
            self.cmds.put(ch)

    def readline(self):
        import queue
        try:
            return self.q.get(timeout=0.3)
        except queue.Empty:
            return b""

    def _emit(self, s):
        self.q.put((s + "\r\n").encode())
        time.sleep(len(s) * 10 / self.baud)

    def _boot(self):
        self.up = False
        r = random.random()
        self._emit("ESP-ROM:esp32s3-20210327")
        self._emit("I (24) boot: ESP-IDF v5.5.5-828-g166f11686a1 2nd stage bootloader")
        self._emit("I (1296) main_task: Calling app_main()")
        mode = "ok"
        if self.scenario == "flaky":
            mode = "ok" if r < 0.5 else ("nosensor" if r < 0.75 else "i2c")
        elif self.scenario == "oldclk":
            mode = "ok"
        if self.scenario == "retry":
            for att in (1, 2):
                if att == 1 and random.random() < 0.7:
                    self._emit("W (1300) ov3660: Camera sensor is not OV3660, PID=0x60")
                    self._emit("E (1302) esp_video_init: failed to detect DVP camera with address=3c")
                    self._emit("CAMINIT,attempt=1,result=ESP_ERR_NOT_FOUND")
                    continue
                self._emit("I (1499) ov3660: Detected Camera sensor PID=0x3660")
                self._emit("I (1613) dvp_ext: DVP Extended camera controller driver is initialized")
                self._emit(f"CAMINIT,attempt={att},result=ok")
                self.up = True
                break
        elif mode == "nosensor":
            self._emit("W (1299) ov3660: Camera sensor is not OV3660, PID=0x0")
            self._emit("E (1302) esp_video_init: failed to detect DVP camera with address=3c")
            self._emit("E (1308) V4L2Device: Failed to open device /dev/video2: No such file or directory")
            self._emit("E (1315) VideoCapture: Device not initialized")
        elif mode == "i2c":
            self._emit("I (1299) ov3660: Detected Camera sensor PID=0x3660")
            self._emit("E (1845) i2c.master: I2C bus is still busy but software timeout detected")
            self._emit("E (1847) ov3660: ov3660_set_format(563): Set common regs failed")
            self._emit("E (1862) V4L2Device: Failed to open device /dev/video2: Device or resource busy")
        else:
            self._emit("I (1299) ov3660: Detected Camera sensor PID=0x3660")
            if self.scenario == "oldclk":
                self._emit("W (1302) esp_video_init: Configured xclk frequency 16000000 is not equal to sensor xclk "
                           "frequency 20000000, the sensor output image may be unexpected")
            self._emit("I (1413) dvp_ext: DVP Extended camera controller driver is initialized")
            self.up = True
        self._emit("FWINFO,viewer=2,modes=qhv,fp=1")
        if self.scenario == "fpfail":
            self._emit("FPINIT,result=fail,code=-1,reason=no response from module (check TX/RX wiring; 5 V power; baud; GPIO pins)")
        else:
            self._emit("FPINIT,result=ok,capacity=162,count=%d,security=3,baud=57600,addr=FFFFFFFF" % len(self.prints))
        self._emit("console ready: r=recognize e=enroll d=delete-last v/h/q=frame(full/half/quarter)")

    def _frame(self, step):
        W = H = 240
        ow, oh = W // step, H // step
        t = time.time()
        bright = int(120 + 60 * __import__("math").sin(t / 4))
        data = bytearray()
        for y in range(0, H, step):
            for x in range(0, W, step):
                r, g, b = min(255, x + bright // 3), min(255, y), 110
                dx, dy = x - 120, y - 110
                if self.face_on and dx * dx + dy * dy < 50 * 50:
                    r, g, b = 225, 185, 155
                if self.face_on and ((x - 102) ** 2 + (y - 95) ** 2 < 49 or (x - 138) ** 2 + (y - 95) ** 2 < 49):
                    r, g, b = 20, 20, 20
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                data += struct.pack(">H", v)
        self._emit(f"FRAME_BEGIN,{ow},{oh},RGB565BE,{len(data)},{step},10")
        if self.face_on:
            self._emit("BOX,68,50,172,170,930,102,95,138,95,120,118,106,145,134,145")
        for off in range(0, len(data), 72):
            self._emit("D," + base64.b64encode(bytes(data[off:off + 72])).decode())
        self._emit("FRAME_END")
        self.frames += 1

    def _run(self):
        import queue
        last_hb = time.time()
        while True:
            try:
                c = self.cmds.get(timeout=0.1)
            except queue.Empty:
                c = None
            if c == "BOOT":
                self._boot(); last_hb = time.time(); continue
            if c in ("q", "h", "v") and self.up:
                self._frame({"q": 4, "h": 2, "v": 1}[c])
            elif c in ("f", "n", "p", "X") and self.scenario == "fpfail":
                self._emit("FPINIT,result=fail,code=-1,reason=no response from module (check TX/RX wiring; 5 V power; baud; GPIO pins)")
            elif c == "p":
                self._emit("FPINIT,result=ok,capacity=162,count=%d,security=3,baud=57600,addr=FFFFFFFF" % len(self.prints))
            elif c == "f":
                self._emit("FP,identify,step=place"); time.sleep(0.8)
                if self.prints and random.random() < 0.8:
                    self._emit("FP,identify,result=match,id=%d,score=%d,wait_ms=%d,capture_ms=%d,search_ms=%d" %
                               (random.choice(self.prints), random.randint(90, 200), random.randint(300, 1500), 310, 48))
                elif self.prints:
                    self._emit("FP,identify,result=nomatch,wait_ms=900,capture_ms=305,search_ms=52")
                else:
                    self._emit("FP,identify,result=nomatch,wait_ms=700,capture_ms=300,search_ms=10")
            elif c == "n":
                nid = len(self.prints)
                self._emit("FP,enroll,step=start,id=%d" % nid)
                for st in ("place1", "remove", "place2", "storing"):
                    self._emit("FP,enroll,step=" + st); time.sleep(0.4)
                self.prints.append(nid)
                self._emit("FP,enroll,result=ok,id=%d,ms=4200" % nid)
                self._emit("FPSTATE,count=%d,capacity=162" % len(self.prints))
            elif c == "X":
                if getattr(self, "_x_armed", 0) > time.time():
                    self.prints = []; self._x_armed = 0
                    self._emit("FP,empty,result=ok"); self._emit("FPSTATE,count=0,capacity=162")
                else:
                    self._x_armed = time.time() + 5
                    self._emit("FP,empty,result=confirm,reason=send X again within 5 s to erase every stored fingerprint")
            elif c == "e":
                self._emit("W (4338) HumanFaceRecognizer: Failed to enroll. No face detected." if not self.face_on
                           else "I (4338) bench_app: id: 1 enrolled.")
            elif c == "r" and self.up:
                self._emit("BENCH,%d,%d,%d,%d,%d,%d,%.3f" % (self.frames, 1, 42000, 310000, 480000, 1, 0.81))
            if self.up:
                self.frames += 1 if random.random() < 0.9 else 0
            if time.time() - last_hb >= 2:
                last_hb = time.time()
                self._emit("HB,%d,%d,%d,%d" % (int(time.time() * 1000) % 10 ** 7, self.frames * 12, 41000, self.frames * 14)
                           if (self.up and self.scenario != "noframes") else "HB,%d,0,0,0" % (int(time.time() * 1000) % 10 ** 7))
            if self.scenario == "drops" and self.up and random.random() < 0.6:
                self._emit("E:RX:115200-14400" if random.random() < 0.9 else "E:RX:115200-100800")
            elif random.random() < 0.02:
                self._emit("E:RX:115200-14400")
            if c == "v" and self.scenario == "drops":
                self._emit("E (435878) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:")


# =====================================================================================================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port"); ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--http", type=int, default=8000); ap.add_argument("--out", default="dashboard_out")
    ap.add_argument("--mode", choices=["q", "h", "v"], default="q", help="q=60px live, h=120px, v=240px")
    ap.add_argument("--no-reset", action="store_true", help="don't reboot the board at start (boot log will be missed)")
    ap.add_argument("--simulate", nargs="?", const="ok", choices=["ok", "flaky", "oldclk", "drops", "retry", "noframes", "fpfail"],
                    help="run against a fake board (ok / flaky camera / old 16 MHz clock)")
    a = ap.parse_args()
    if a.simulate:
        ser = FakeSerial(a.baud if a.baud > 115200 else 460800, a.simulate)
        a.baud = ser.baud
    else:
        if not a.port:
            ap.error("need --port (or --simulate)")
        import serial
        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = a.port, a.baud, 0.3
        ser.dtr = False; ser.rts = False  # opening must not reset the board
        try:
            ser.open()
        except Exception as e:  # noqa
            print(f"cannot open {a.port}: {e}\nIs `idf.py monitor` still running? Only one program can hold the port.")
            sys.exit(1)
    d = Dashboard(ser, a.baud, a.mode, a.out)
    threading.Thread(target=d.reader, daemon=True).start()
    threading.Thread(target=d.streamer, daemon=True).start()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.http), make_handler(d))
    print(f"dashboard: http://localhost:{a.http}   (Ctrl+C to quit)")
    if not a.no_reset and not a.simulate:
        threading.Timer(1.0, d.reboot).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    d.stop = True


if __name__ == "__main__":
    main()
