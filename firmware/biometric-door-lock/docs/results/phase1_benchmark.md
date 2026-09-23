# Phase 1 benchmark - on-device face detect + recognize

Setup: ESP32-S3 N16R8 at 240 MHz, octal PSRAM 80 MHz, OV2640 (240x240 RGB565), ESP-WHO/ESP-DL human face detect +
recognize, one enrolled person, camera streaming, dashboard closed, 50 valid runs per condition (`tools/bench.py`).
Raw CSVs: `data/phase1/` (copy `bright.csv`, `dim.csv`, `backlit.csv` there).

| Condition | detect ms (mean / p95) | recognize ms (mean / p95) | recognized (of valid) | similarity mean / min | attempts with no face |
|---|---|---|---|---|---|
| bright indoor | 44.6 / 44.7 | 267.5 / 267.6 | 49/50 (98%) | 0.63 / 0.53 | 5 of 55 (9%) |
| dim indoor | 44.5 / 44.6 | 267.5 / 267.6 | 49/50 (98%) | 0.65 / 0.50 | 53 of 103 (51%) |
| backlit | 44.6 / 44.6 | 267.5 / 267.6 | 50/50 (100%) | 0.70 / 0.55 | 8 of 58 (14%) |

## Reading the numbers
- **Compute latency = detect + recognize = about 312 ms**, the same in every condition (neural inference time does not
  depend on image content). Add up to one frame interval (~50 ms at 20 fps) for a capture-to-decision estimate.
  Espressif's published figures were ~17-56 ms detect and ~287-554 ms recognize; the measurement agrees.
- The tool's trigger-to-result "e2e" column (about 287 ms) is **not** a latency (it was shorter than detect + recognize);
  see ADR-006. Do not quote it.
- **System-level success (recognized / all attempts):** bright 89%, backlit 86%, dim 48%. Detection, not recognition, is the
  weak stage in dim light. Some no-face attempts may be moments when the subject was not in frame.
- Genuine similarity scores sit close to the accept threshold (0.5). All trials were genuine, so **FAR is unmeasured**.
- Camera cold-start (fingerprint pass -> first decision) was **not** measured: the camera was already streaming.

## Follow-ups
Low-light handling (supplementary light on fingerprint pass) - Phase 3. Impostor trials - Phase 7. Latency under Wi-Fi
load - Phase 5. Contract latency target - needed.
