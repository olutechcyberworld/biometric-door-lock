#!/usr/bin/env python3
"""Starts the dashboard against a simulated board and checks the API end to end (camera, fingerprint, boot test)."""
import json, os, random, subprocess, sys, tempfile, time, urllib.request
root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
port = random.randint(20000, 40000)
out = tempfile.mkdtemp()
proc = subprocess.Popen([sys.executable, os.path.join(root, "tools/dashboard.py"), "--simulate", "ok", "--http", str(port),
                         "--out", out], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
base = f"http://127.0.0.1:{port}"
def state(): return json.loads(urllib.request.urlopen(base + "/api/state").read())
def post(p): return urllib.request.urlopen(urllib.request.Request(base + p, method="POST")).read().decode()
try:
    for _ in range(50):
        try: state(); break
        except Exception: time.sleep(0.2)
    time.sleep(6)
    st = state()
    assert st["cam"]["status"] in ("STREAMING", "OK"), st["cam"]
    assert st["frame"]["n"] > 0, "no frame rendered"
    png = urllib.request.urlopen(base + "/frame.png").read(); assert png[:8] == b"\x89PNG\r\n\x1a\n"
    assert st["fp"]["supported"] and st["fp"]["init"] == "ok", st["fp"]
    post("/api/cmd?c=fp_enroll"); time.sleep(3.5)
    assert state()["fp"]["count"] == 1, state()["fp"]
    post("/api/cmd?c=fp_identify"); time.sleep(2)
    assert state()["fp"]["stats"]["match"] + state()["fp"]["stats"]["nomatch"] >= 1
    print("dashboard smoke test ok")
finally:
    proc.terminate()
