#!/usr/bin/env python3
"""Phase 1 benchmark driver for the ESP-WHO bench firmware.

Live:    python bench.py --port COM5 --n 50 --label bright_indoor --out bright.csv
Replay:  python bench.py --replay saved_monitor_log.txt --label bright_indoor

Firmware prints:  BENCH,seq,faces,detect_us,recog_us,e2e_us,id,sim
Only rows with faces>=1 are counted (a 0-face frame consumes the trigger and says nothing
about recognition cost). e2e includes the wait for the next camera frame.
"""
import argparse, csv, re, statistics, sys, time

ROW = re.compile(r"BENCH,(\d+),(\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?[\d.]+)")
FIELDS = ["seq", "faces", "detect_us", "recog_us", "e2e_us", "id", "sim"]


def parse_line(line):
    m = ROW.search(line)
    if not m:
        return None
    v = m.groups()
    return {k: (float(x) if k == "sim" else int(x)) for k, x in zip(FIELDS, v)}


def pct(vals, p):
    s = sorted(vals)
    if not s:
        return float("nan")
    k = (len(s) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def summarize(rows, label):
    valid = [r for r in rows if r["faces"] >= 1]
    nofc = len(rows) - len(valid)
    print(f"\n=== {label}: {len(valid)} valid runs ({nofc} no-face attempts excluded) ===")
    if not valid:
        return
    for name in ("detect_us", "recog_us", "e2e_us"):
        v = [r[name] / 1000.0 for r in valid if r[name] >= 0]
        if not v:
            continue
        print(f"{name[:-3]:>7} ms  mean {statistics.mean(v):7.1f}  min {min(v):7.1f}  "
              f"p50 {pct(v,50):7.1f}  p95 {pct(v,95):7.1f}  max {max(v):7.1f}")
    other = [(r["e2e_us"] - r["detect_us"] - r["recog_us"]) / 1000.0 for r in valid if r["e2e_us"] >= 0]
    if other:
        print(f"  frame-wait+sched (e2e-detect-recog) ms  mean {statistics.mean(other):.1f}  p95 {pct(other,95):.1f}")
    hits = [r for r in valid if r["id"] >= 0]
    sims = [r["sim"] for r in hits]
    print(f"recognized {len(hits)}/{len(valid)} ({100.0*len(hits)/len(valid):.0f}%)"
          + (f"  sim mean {statistics.mean(sims):.2f} min {min(sims):.2f}" if sims else ""))


def live(args):
    import serial  # pip install pyserial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, args.baud, 0.2
    s.dtr = False  # avoid auto-reset of the ESP32-S3 on open
    s.rts = False
    s.open()
    rows, sent, last_tx = [], 0, 0.0
    deadline = time.time() + args.n * args.gap * 3 + 30
    valid = 0
    while valid < args.n and time.time() < deadline:
        if time.time() - last_tx >= args.gap:
            s.write(b"r")
            sent += 1
            last_tx = time.time()
        line = s.readline().decode(errors="replace")
        r = parse_line(line)
        if r:
            rows.append(r)
            valid = sum(1 for x in rows if x["faces"] >= 1)
            print(f"\r{valid}/{args.n} valid", end="", flush=True)
    s.close()
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--n", type=int, default=50, help="valid (face-present) runs to collect")
    ap.add_argument("--gap", type=float, default=1.5, help="seconds between 'r' triggers")
    ap.add_argument("--label", default="run")
    ap.add_argument("--out", help="write raw rows to CSV")
    ap.add_argument("--replay", help="parse an existing monitor log instead of using the serial port")
    a = ap.parse_args()

    if a.replay:
        rows = [r for r in (parse_line(l) for l in open(a.replay, errors="replace")) if r]
    elif a.port:
        rows = live(a)
    else:
        ap.error("need --port or --replay")

    if a.out:
        with open(a.out, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            w.writerows(rows)
    summarize(rows, a.label)


if __name__ == "__main__":
    main()
