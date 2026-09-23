# Two-Factor Biometric Door Lock
## System Architecture, Design Summary & Bill of Materials (Revision 2)
**Prepared by:** Olumide (Abdulrasheed Olumide)
**Document purpose:** HND final-year project — revised architecture and BOM
**Status:** Design finalized on paper — pending on-hardware validation
**Supersedes:** Rev 1 (three-factor, dual-microcontroller design)

---

## 0. Revision Summary

Rev 1 proposed a three-factor system (fingerprint + phone-based face recognition + phone-based voice recognition) split across an ESP32 and an ESP32-Cam, with the homeowner's phone performing all face/voice inference and an MQTT broker handling notifications and remote recovery.

This revision reduces scope deliberately, for two reasons: (1) HND-level buildability and defensibility, and (2) removing architectural risk identified during design review — specifically, the phone's role as an always-available HTTP server in the authentication critical path, and a recovery flow that could be triggered by simply making the phone unreachable.

| Change | Rev 1 | Rev 2 |
|---|---|---|
| Factors | Fingerprint + face + voice | Fingerprint + face |
| Microcontrollers | ESP32 + ESP32-Cam (2 boards) | ESP32-S3 N16R8 (1 board, integrated camera) |
| Face recognition location | Homeowner's phone (TFLite) | On-device (ESP-WHO / ESP-DL) |
| Voice verification | Homeowner's phone (GMM-MFCC) | Removed |
| Phone role | In the authentication critical path | Removed from authentication; used only for override |
| Remote channel | MQTT (HiveMQ free tier) | Firebase (override/recovery only, non-critical) |
| Recovery trust model | Secondary trusted device + fingerprint | Dashboard login + PIN + fingerprint (recovery); local app + PIN + fingerprint (override) |
| Mobile apps | 2 (primary + secondary) | 1 (owner app — override + enrollment only, not in the auth path) |

---

## 1. System Overview

The system is a two-factor biometric door access control unit for residential deployment, combining fingerprint and facial recognition to gate a solenoid-driven door lock. All biometric acquisition, matching, and access decisioning run entirely on a single ESP32-S3 microcontroller — no phone and no cloud service is involved in a normal unlock event. A companion Android app and a Firebase-hosted dashboard exist solely to support two clearly separated bypass paths (override and recovery), described in Section 1.3.

### 1.1 Authentication Flow (Sequential — no factor skippable)

```
Finger contact
      │
      ▼
[1] Fingerprint — AS608 on-module match (UART)
      │ Pass
      ▼
[2] Camera wakes — ESP32-S3 on-device face detection + recognition
      │       (ESP-WHO / ESP-DL, local inference — no network hop)
      │ Pass
      ▼
[3] Authentication FSM confirms both factors
      │
      ▼
Solenoid lock actuated — access granted
```

Fingerprint gates the camera waking at all — a cheap, fast rejection stage before the more expensive recognition step runs. Any failure at any stage → immediate denial, event logged locally.

### 1.2 Override Flow (owner present, phone functional)

Used when the owner is physically at the door but a factor (typically face recognition — poor lighting, camera fault, etc.) is failing.

```
Owner opens app, sends override command over LOCAL Wi-Fi
      │  (app and ESP32-S3 on the same home network — no
      │   internet dependency for this path)
      ▼
App prompts for PIN → PIN sent to ESP32-S3
      │  (never sent raw — hashed/challenge-response over the local link)
      ▼
ESP32-S3 verifies PIN hash on-device (source of truth, not the app)
      │ Valid
      ▼
Supervised window opens → fingerprint required at door
      │ Match within window
      ▼
Solenoid lock actuated — access granted
```

### 1.3 Recovery Flow (phone unavailable — lost, damaged, or compromised)

Used when the owner's phone cannot be used to trigger override at all. This is a distinct failure mode from override, not a duplicate of it: override assumes a working phone on the home network; recovery assumes it does not exist.

```
Owner logs into Firebase-hosted recovery dashboard
      │  (any internet connection — not required to be on home Wi-Fi)
      ▼
Owner enters PIN in dashboard
      │
      ▼
Recovery request (session + PIN) relayed to ESP32-S3
      │  Valid session + valid PIN hash
      ▼
Supervised window opens → fingerprint required at door
      │ Match within window
      ▼
Solenoid lock actuated — access granted
```

Recovery therefore requires three independent things to align: a valid dashboard session (something the owner has access to), a correct PIN (something the owner knows), and a fingerprint match at the door (something only the owner is). No single compromised credential — including the Firebase login alone — is sufficient to unlock the door.

---

## 2. System Architecture

### 2.1 Architectural Layers

| Layer | Components | Role |
|---|---|---|
| Hardware | ESP32-S3 N16R8 (integrated/companion camera), AS608, Relay, Lock, LED, Buzzer | Sensor acquisition, on-device inference, and actuation |
| Firmware | FreeRTOS on ESP32-S3 | Authentication FSM, fingerprint driver, on-device face recognition, PIN verification, override/recovery handling, relay control |
| Application | Flutter Android app (single app, owner-only) | Local override command + PIN entry; enrollment; not in the authentication critical path |
| Cloud (non-critical) | Firebase | Recovery dashboard hosting + auth; never gates a normal unlock |

The door must never depend on Firebase being reachable for a normal unlock — this was the one architectural principle carried forward deliberately from Rev 1's MQTT design, even though MQTT itself was dropped.

### 2.2 FreeRTOS Task Breakdown (ESP32-S3 Firmware)

| Task | Core (proposed) | Responsibility |
|---|---|---|
| Authentication FSM | Core 1 | Central state machine — owns all authentication, override, and recovery transitions |
| Fingerprint Interface | Core 0 | UART comms with AS608, delivers pass/fail to FSM |
| Camera + Face Recognition | Core 1 | Frame capture, ESP-WHO detect → recognize pipeline, delivers pass/fail to FSM |
| Local Wi-Fi / App Server | Core 0 | Serves override requests from the owner app over local Wi-Fi |
| Firebase Client | Core 0 | Polls/receives recovery requests; never touches relay directly |
| Relay Control | Core 0 | Drives relay GPIO and indicator GPIO on FSM command only |

> **Open item:** camera/inference and Wi-Fi handling are provisionally split across cores to avoid contention during a face-recognition pass while the app or dashboard is active. This needs validating on hardware — inference latency under simultaneous Wi-Fi load has not yet been measured (see Section 4).

### 2.3 Application Summary

**Owner App (Flutter, Android)**
- Used for enrollment and for the local override path only
- Sends override command + PIN to ESP32-S3 over local Wi-Fi (same network only)
- PIN is hashed/challenge-response before transmission — never sent raw
- Has no role in normal (fingerprint + face) authentication

**Recovery Dashboard (Firebase-hosted, web)**
- Login-gated
- Accepts PIN entry, relays recovery request + session to ESP32-S3
- Accessible from any internet connection, not tied to the home network

### 2.4 Communication Channels

| Channel | Protocol | Data Carried | Network Zone |
|---|---|---|---|
| AS608 → ESP32-S3 | UART 57600 8N1 | Binary pass/fail | On-board |
| Camera → ESP32-S3 inference | Internal (DVP/on-chip) | Raw frame, consumed locally — never transmitted | On-board |
| Owner App ↔ ESP32-S3 | Local Wi-Fi | Override command, hashed PIN | Local network only |
| Firebase ↔ ESP32-S3 | Internet | Recovery session + PIN, non-critical logs | Internet (non-critical path) |
| Relay → Solenoid Lock | GPIO / 12V switched | Actuation signal | On-board |

> **Privacy note:** Raw biometric data (face frames, fingerprint templates) never leaves the ESP32-S3 or the AS608's own internal storage. Nothing biometric is ever transmitted over Wi-Fi or the internet.

### 2.5 Security Architecture Summary

| Control | Implementation |
|---|---|
| Factor enforcement | Sequential FSM — no stage skippable from door or network |
| PIN protection | Never transmitted raw; hashed/challenge-response over local link; hash stored on ESP32-S3 flash; verified on-device (app is transport only, not the source of truth) |
| PIN brute-force protection | Attempt lockout with cooldown, enforced in FSM, independent of the fingerprint/face path |
| Override vs. recovery separation | Override = local network + app + PIN + fingerprint (phone functional). Recovery = internet + dashboard login + PIN + fingerprint (phone unavailable). No single credential unlocks either path alone. |
| Template protection | Fingerprint templates in AS608 internal flash; face embeddings stored on ESP32-S3, never transmitted |
| Cloud dependency | Firebase used for recovery only; normal unlock has zero internet dependency |

### 2.6 Security Scope (explicitly out of scope for this prototype)

Stated deliberately, not as an oversight:

- **Flash encryption / secure boot** on the ESP32-S3 — not enabled for this prototype. A known limitation against an attacker with physical chip access.
- **TLS on the local app ↔ ESP32-S3 link** — not implemented; PIN is protected via hash/challenge-response instead of transport encryption. Acceptable for a local-network prototype; would be required for a production deployment.
- **Defense against a sophisticated attacker with physical access to the board** — out of scope. This prototype defends against the threat model relevant to its thesis (multi-factor vs. single-factor access control), not against nation-state-level adversaries.

The test applied to each item above: does skipping it undermine the project's core thesis (multi-factor beats single-factor), or does it only mean the prototype isn't hardened against a much stronger attacker class? Only items that would undermine the thesis were treated as required.

---

## 3. Bill of Materials

### 3.1 Hardware Components

| # | Component | Specification | Qty | Unit (NGN est.) | Total (NGN est.) |
|---|---|---|---|---|---|
| 1 | ESP32-S3 N16R8 dev board (with camera) | 16MB flash, 8MB octal PSRAM, integrated/companion camera module | 1 | — | — |
| 2 | AS608 fingerprint sensor | Optical, UART, on-module template storage and matching, up to 1000 templates | 1 | — | — |
| 3 | 5V single-channel relay module | GPIO-triggered, rated ≥10A/250VAC | 1 | — | — |
| 4 | 12V DC solenoid door lock | Electric drop bolt or electric strike, 12V DC | 1 | — | — |
| 5 | 12V DC power supply | ≥2A output, wall-mount adapter | 1 | — | — |
| 6 | 5V DC buck converter / regulator | LM2596 or equivalent, 12V→5V for ESP32-S3 supply | 1 | — | — |
| 7 | Status LED (green + red) | 5mm through-hole, green and red (1 each) | 2 | — | — |
| 8 | Piezo buzzer | 5V passive, through-hole | 1 | — | — |
| 9 | Push button (configuration) | Momentary tactile, through-hole | 1 | — | — |
| 10 | Resistors (LED current limiting) | 220Ω – 470Ω, ¼W | 4 | — | — |
| 11 | Jumper wires / DuPont cables | Male-male, male-female assorted | 1 set | — | — |
| 12 | Breadboard or custom PCB | 830-point breadboard or custom PCB for final build | 1 | — | — |
| 13 | Project enclosure / housing | ABS enclosure, sized for board + sensors | 1 | — | — |
| 14 | USB cable (per board's connector type) | For ESP32-S3 programming and debug | 1 | — | — |

**Removed from Rev 1:** second ESP32-Cam module, MEMS microphone module (voice factor dropped). ESP32-S3 board already purchased.

### 3.2 Software Components (Zero Additional Cost)

| Component | Version / Source | Cost |
|---|---|---|
| ESP-IDF | Espressif official — open source | Free |
| FreeRTOS | Bundled with ESP-IDF | Free |
| ESP-WHO / ESP-DL | Espressif official — open source, on-device face detection + recognition | Free |
| Flutter SDK | Google — open source | Free |
| Firebase (recovery dashboard hosting + auth) | Google — free tier | Free |
| Owner Android APK | Built in-house | Included in development effort |

**Removed from Rev 1:** TensorFlow Lite for Android, MobileFaceNet phone-side model, GMM-MFCC voice pipeline, HiveMQ/MQTT broker, secondary app.

### 3.3 Development and Effort (For Planning)

| Item | Notes |
|---|---|
| Firmware development | FreeRTOS task architecture, AS608 driver, ESP-WHO integration, PIN hash verification, authentication/override/recovery FSM, relay control |
| Owner app development | Flutter Android, local override command + PIN entry, enrollment flow |
| Recovery dashboard | Firebase-hosted login + PIN entry + request relay |
| System integration and testing | End-to-end authentication flow, override flow, recovery flow, latency benchmarking, FAR/FRR evaluation |
| Enclosure and assembly | PCB or breadboard mounting, enclosure fitting, cable management |
| Documentation | Architecture, BOM, security scope, test report |

### 3.4 Consumables and Miscellaneous

| Item | Notes |
|---|---|
| Solder and flux | For final PCB assembly |
| Heat shrink tubing | Cable insulation |
| Cable ties | Cable management inside enclosure |
| Screws and standoffs | PCB and enclosure mounting |

---

## 4. Open Items / Remaining Unknowns

| # | Item | Status |
|---|---|---|
| 1 | On-device face recognition latency, measured on the actual board | Espressif's published ESP-DL figures for ESP32-S3 (~287–554ms recognition, ~17–56ms detection) used as a design estimate; still needs first-hand measurement with the owner's enrolled face and real lighting |
| 2 | CPU/core allocation between camera inference and Wi-Fi/network handling | Provisional split (Section 2.2) — needs validation under simultaneous load |
| 3 | Power supply headroom under camera + Wi-Fi concurrent draw | Not yet tested |
| 4 | Number of enrolled users/faces to support | Client/scope decision — assumed single owner unless stated otherwise |
| 5 | Enclosure specification | Wall-mount vs. standalone — pending preference |
| 6 | NGN unit prices for BOM (Table 3.1) | Pending supplier quotes |

---

*Revision 2 prepared following architecture review. All technical decisions remain subject to validation on actual hardware prior to final submission.*
