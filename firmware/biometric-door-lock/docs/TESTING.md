# Testing

## Host tests (no hardware, no ESP-IDF)
`tests/run_host_tests.sh` runs: layout check; Python syntax; the AS608 driver against a simulated module (datasheet packet
vectors, probe, enroll, identify, comm errors, free-id/delete/erase); firmware frame dump -> dashboard parser round trip
(all three sizes, both byte orders); and a dashboard smoke test against a simulated board.
`python tools/dashboard.py --simulate [ok|flaky|oldclk|drops|retry|noframes|fpfail]` shows the dashboard without a board.

## First build in this layout (do this before trusting it)
1. `scripts/import_lock.sh`, `scripts/setup.sh`, `scripts/build.sh`. The first build downloads components and is slow.
2. Flash, start the dashboard. Compare with the old tree: boot row should show the sensor PID and `CAMINIT,attempt=1`.
3. If the build fails, keep using the old ESP-WHO checkout (still works) and send the first error line.
   Likeliest trouble spots: a component path (`scripts/check_layout.py` lists missing ones), the pinned versions
   (`dependencies.lock`), and the fingerprint files (turn off `Door lock > Enable the AS608 fingerprint interface`
   in menuconfig to isolate them).

## Face benchmark (Phase 1 procedure)
Enroll 2-3 times at ~40 cm in good light, then close the dashboard. For each lighting condition:
`scripts/bench.sh <label>` (50 valid runs, face in frame). CSVs go to `data/phase1/`. Report both the valid-run
statistics and the system-level success rate (recognized / all attempts). Save dashboard snapshots as lighting evidence.

## Camera boot test
Dashboard > "Boot test x10": reboots 10 times and tallies OK / retried / failed. Repeat after any hardware change.

## Fingerprint bring-up
1. Wire per [WIRING.md](WIRING.md); check the module's TX idle voltage first.
2. Flash. Dashboard "Fingerprint" card must show `Sensor OK` with capacity and stored count. If it says NOT RESPONDING,
   follow the banner (TX/RX crossed? 5 V? baud 57600? GPIOs in menuconfig?), then press Probe.
3. Enroll 3+ fingers (button "Enroll new", follow the on-screen prompts).
4. Identify: genuine trials (enrolled finger) and impostor trials (other people's fingers). The card records the result,
   score and `capture + search` time. Save the numbers for FAR/FRR.
5. Failure injection: unplug the module (banner appears), replug + Probe (recovers).
