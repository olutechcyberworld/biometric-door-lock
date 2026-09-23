# Decision log

Each entry: decision, why, evidence, and what would change it.

## ADR-001 Native ESP-IDF (not Arduino, not PlatformIO)
ESP-WHO/ESP-DL run on ESP-IDF and use its component manager; the FreeRTOS task/core design in the architecture document
is native there; secure boot / flash encryption (future hardening) are IDF features.
Change if: the vendor stack moves to something PlatformIO supports better.

## ADR-002 Vendor the ESP-WHO components; declare the BSP in the manifest
ESP-WHO's example relies on an IDF extension (`IDF_EXTRA_ACTIONS_PATH`) that only works for directories named like its
own examples; that caused a build failure during bring-up. The project vendors only the components it links
(`firmware/third_party/esp-who`, our edits listed in VENDORED.md) and lists the board package (`espressif/esp32_s3_eye`)
in `main/idf_component.yml`, so `idf.py set-target esp32s3 && idf.py build` is all that is needed.
Dependency versions are pinned by `firmware/dependencies.lock` (commit it after the first good build).

## ADR-003 Camera: OV2640 instead of OV3660
Evidence (bring-up log): with the OV3660 the sensor ID read was intermittently wrong (PID 0x0 / 0x60), the DVP driver
rejected most frames (`E:RX:115200-14400` = frames of the wrong byte count) and, after my custom camera start-up code,
no frames arrived at either 16 or 20 MHz. With an OV2640 on Espressif's unmodified start-up path the camera delivered
about 20 fps with zero dropped frames. Caveat: the intermittent camera-bus failure remained (ADR-004), so the sensor
was not the only problem.

## ADR-004 Recover from camera init failure by restarting
Symptom (both sensors): `I2C transaction timeout detected` while the sensor register list is written, roughly half of the
boots on the dev board. A software restart clears it. `frame_cap_pipeline.cpp` restarts up to 8 times in a row (counter
in RTC memory, printed as `CAMINIT,attempt=N`). This hides a hardware weakness; the proper fix is on the board (bus
pull-ups, wiring, connector, power). Track it as a known limitation until then.

## ADR-005 Debug through a browser dashboard using a text frame tap
The detector frame, boxes and stats are printed as text lines (base64, one self-contained line each) so they survive
CRLF translation and interleaved log lines. `tools/dashboard.py` shows the live image, per-boot camera outcome, drop
counters, watchdog/crash banners, and the fingerprint card. Frame dumps block the detector, so throughput measurements
must be taken with the stream paused (or the dashboard closed). The dump yields to the idle task (task watchdog).

## ADR-006 Latency is reported as detect + recognize
`BENCH` lines also carry a trigger-to-result time, but it starts when the request arrives while the detection it uses
began earlier, so it can be shorter than detect + recognize (it was, in the Phase 1 data). Use compute time
(detect 44.5 ms + recognize 267.5 ms) plus up to one frame interval (~50 ms at 20 fps) when quoting latency.

## ADR-007 Fingerprint driver is transport-independent and unit-tested
`as608.c` never touches hardware; `as608_uart.c` maps it onto an ESP32 UART. This allowed testing the protocol against a
simulated module (including datasheet packet vectors) before the sensor is wired. The AS608 stores templates inside the
module; nothing biometric leaves it (matches the architecture's privacy note).

## ADR-008 Face recognizer threshold stays at the library default (0.5) for now
Genuine similarity in Phase 1: mean 0.63-0.70, minimum 0.50-0.55 (all trials were the enrolled person). The margin to the
threshold is thin and FAR is unmeasured; revisit in Phase 7 with impostor data.
