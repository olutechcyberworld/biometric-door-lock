#!/usr/bin/env python3
"""Firmware frame dump -> dashboard parser round trip (host only). Fails loudly if the wire format drifts."""
import os, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, "../../.."))
sys.path.insert(0, os.path.join(root, "tools"))
import dashboard as D

exe = sys.argv[1]
def run(mode, be):
    out = subprocess.run([exe, str(mode), str(be)], capture_output=True, text=True, check=True).stdout
    p, frames = D.FrameParser(), []
    for line in out.splitlines():
        kind, payload = p.feed(line.strip())
        if kind == "frame":
            frames.append(payload)
        assert kind != "drop", payload
    return frames

bad = 0
for mode, (w, step) in {1: (240, 1), 2: (120, 2), 3: (60, 4)}.items():
    for be, pix in ((1, "RGB565BE"), (0, "RGB565LE")):
        fr = run(mode, be)
        ok = len(fr) == 1 and fr[0]["w"] == w and fr[0]["h"] == w and fr[0]["pix"] == pix and fr[0]["step"] == step \
            and len(fr[0]["data"]) == w * w * 2 and len(fr[0]["boxes"]) == 1 and fr[0]["boxes"][0]["box"] == [68, 50, 172, 170] \
            and len(fr[0]["boxes"][0]["kp"]) == 10
        img, W, H, st = D.render(fr[0]) if fr else (None, 0, 0, None)
        ok = ok and st is not None and 120 < st["mean"] < 140          # synthetic scene has a known brightness
        print(f"mode {mode} {pix}: {'ok' if ok else 'FAIL'}")
        bad += 0 if ok else 1
sys.exit(1 if bad else 0)
