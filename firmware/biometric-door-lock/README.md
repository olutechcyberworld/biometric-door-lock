# Two-Factor Biometric Door Lock

ESP32-S3 door lock that gates a solenoid behind **fingerprint (AS608) then face recognition (on-device, ESP-WHO/ESP-DL)**.
No phone or cloud is in the normal unlock path; a local app and a Firebase dashboard exist only for the override and
recovery flows. Full design: [docs/architecture_and_bom_v2.md](docs/architecture_and_bom_v2.md).

## Status

| Phase | Scope | State |
|---|---|---|
| 1 | Hardware bring-up + on-device face recognition benchmark | **Done** (OV2640; results in [docs/results/phase1_benchmark.md](docs/results/phase1_benchmark.md)) |
| 2 | AS608 fingerprint over UART | **Code written and tested against a simulated module; hardware bring-up pending** |
| 3 | Core FSM (fingerprint gates camera, relay) | Not started |
| 4 | PIN hash + verification on-device | Not started |
| 5 | Local override path (app + PIN + fingerprint) | Not started |
| 6 | Firebase recovery path | Not started |
| 7 | Integration tests, FAR/FRR evaluation | Not started |
| 8 | Enclosure, documentation, defense prep | Not started |

Details and open items: [docs/ROADMAP.md](docs/ROADMAP.md). Why things are the way they are: [docs/DECISIONS.md](docs/DECISIONS.md).

## Layout

```
firmware/            ESP-IDF project (ESP32-S3, IDF 5.5.x)
  main/              app_main, camera pipeline, fingerprint task, console commands
  components/as608/  AS608 driver (platform independent) + ESP UART glue + host unit tests
  third_party/       vendored ESP-WHO components (see VENDORED.md there for our modifications)
tools/               dashboard.py (browser debug UI), bench.py (face benchmark)
scripts/             env / setup / build / flash / dashboard / bench helpers
tests/               host-only tests (no board, no ESP-IDF)
docs/                architecture + BOM, roadmap, decisions, wiring, testing, results, evidence
data/                raw measurement CSVs (data/phase1/...)
app/  cloud/         placeholders for Phase 5 (Flutter app) and Phase 6 (Firebase)
```

## Quick start (Linux)

```bash
pip install -r tools/requirements.txt          # pyserial, for the tools
source scripts/env.sh                          # every new terminal (loads ESP-IDF, sets defaults)
scripts/import_lock.sh                         # first time only: reuse the dependency versions that already built
scripts/setup.sh                               # first time only: set-target + generate sdkconfig
scripts/build.sh
scripts/flash.sh                               # port from $DOORLOCK_PORT (default /dev/ttyACM0)
scripts/dashboard.sh                           # then open http://localhost:8000
```

The dashboard owns the serial port, so flash first, then start it. It reboots the board on start to capture the boot log.
Use `scripts/bench.sh <label>` for benchmark runs (dashboard closed).

Host tests (no hardware): `tests/run_host_tests.sh`.

## Console commands (serial or dashboard buttons)

| Key | Action |
|---|---|
| `r` `e` `d` | face: recognize / enroll / delete last |
| `v` `h` `q` | dump the next detector frame: full 240 px / half / quarter |
| `f` `n` `p` `X` | fingerprint: identify / enroll next free slot / probe sensor / erase all (send `X` twice) |

## Known limitations (prototype scope)

See section 2.6 of the architecture document, plus: camera bus initialisation fails intermittently on the dev board and
the firmware recovers by restarting (ADR-004); face detection is weak in dim light (Phase 1 results); false-accept rate is
not measured yet; the anti-spoofing (liveness) check is out of scope.

## Third-party code
`firmware/third_party/esp-who` is vendored from Espressif's ESP-WHO (Apache-2.0, license included; our edits are listed in
its VENDORED.md). `tests/host/frame_dump/stub/dl_image_define.hpp` is a copy of a header from Espressif's ESP-DL
(Apache-2.0), used only by the host tests. ESP-IDF, ESP-DL and the board support package are fetched by the component
manager at build time.
