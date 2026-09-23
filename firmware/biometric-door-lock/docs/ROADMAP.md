# Roadmap and status

Order follows the implementation plan agreed after architecture review (Rev 2).

## Phase 1 - Hardware bring-up + face recognition benchmark: DONE
- Toolchain: native ESP-IDF 5.5 (ADR-001). Board: ESP32-S3 N16R8 with DVP camera header, **OV2640** (ADR-003).
- Results ([details](results/phase1_benchmark.md)): detect 44.5 ms + recognize 267.5 ms = about 312 ms compute per attempt,
  identical across lighting conditions. Detection (not recognition) fails in dim light: 51% of dim-light attempts found no face.
- Not yet covered: FAR (needs impostor attempts), camera cold-start latency (needed by the fingerprint-gates-camera flow).

## Phase 2 - Fingerprint (AS608 over UART): CODE READY, HARDWARE PENDING
- Driver + task + dashboard card + console commands are in. The driver passes host tests against a simulated module
  (datasheet packet vectors, enroll, identify, error paths). Nothing has run against the real module yet.
- Bring-up checklist: [TESTING.md](TESTING.md#fingerprint-bring-up). Wiring: [WIRING.md](WIRING.md).
- Exit criteria: FPINIT ok; enroll 3+ fingers; identify genuine/impostor trials recorded with `capture_ms + search_ms`.

## Phase 3 - Core FSM: NOT STARTED
Fingerprint pass wakes the camera, sequential pass/fail, relay actuation. Must measure **camera cold-start to first
decision** and settle the Section 2.2 core/task allocation on hardware. Decide low-light handling (supplementary light).

## Phase 4 - PIN hash + verification: NOT STARTED
Implement and test in isolation (host tests first), attempt lockout with cooldown in the FSM.

## Phase 5 - Local override: NOT STARTED   |   Phase 6 - Firebase recovery: NOT STARTED
Wi-Fi arrives here: re-run the Phase 1 latency benchmark under Wi-Fi load (open item 2 in the architecture document).

## Phase 7 - Integration and FAR/FRR: NOT STARTED
Impostor protocol (other people, photo on a phone screen), threshold study (face recognizer default similarity
threshold 0.5; genuine scores in Phase 1 averaged 0.63-0.70 with minimums at 0.50-0.55), lockout behaviour.

## Phase 8 - Enclosure, documentation, defense: NOT STARTED

## Open items carried from the architecture document
| # | Item | State |
|---|---|---|
| 1 | Face recognition latency on the real board | **Closed**: measured, see Phase 1 results |
| 2 | Core allocation under camera + Wi-Fi load | Open (Phase 3 and 5) |
| 3 | Power headroom, camera + Wi-Fi + solenoid | Open |
| 4 | Number of enrolled users | Open (assumed one owner) |
| 5 | Enclosure specification | Open |
| 6 | NGN unit prices in the BOM | Open |
| new | Contract latency target | **Unknown - needs the number from the contract/spec** |
