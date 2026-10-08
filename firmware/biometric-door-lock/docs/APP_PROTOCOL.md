# App <-> Firmware Protocol (the "Firmware Contract" the app was built against)

Status: **reverse-engineered from `biometric-doorlock-app` on 2026-09-28.** No written Sections 4/5 existed in
either repo before this file - the app code was the only spec. This document is now the single source of truth
for both sides; when they disagree, fix the code to match this file, not the other way round.

Status: implemented in firmware - Wi-Fi + provisioning (`wifi_manager`), discovery and the HTTP server (`device_link`),
PIN endpoints (`main/pin_api.cpp`), `/override` (`main/override_api.cpp`) and the `/session` WebSocket
(`main/session_ws.cpp`, `main/user_db.cpp`). Where the firmware had to extend the original contract, the extension is
marked **(added)** below.

## Transport
- Plain HTTP + WS, local Wi-Fi only, no TLS - matches Rev 2 doc Section 2.6 (TLS explicitly out of scope for the
  prototype).
- Device advertises as `doorlock.local` (mDNS) - the app's default host. If mDNS resolution isn't reliable on
  your router, the app also accepts a raw IP entered in Settings.

## Discovery (app finds the lock without knowing its IP)
The lock's IP is assigned by whatever network it joins and can change, so the app never relies on a typed IP.
- `GET /whoami` (no auth, port 80) -> `200 {"device":"doorlock","id":"<12 hex mac>","fw":"...","ip":"a.b.c.d"}`
- UDP broadcast beacon to port **4210** (directed + 255.255.255.255) every ~3 s while online and immediately after
  joining: `{"t":"doorlock","id":"<12 hex mac>","ip":"a.b.c.d","port":80}`
- mDNS: `doorlock.local` plus service `_doorlock._tcp` (TXT `id`, `fw`). Best-effort only: on an Android phone that
  is itself the hotspot, mDNS usually does not work, so the app must not depend on it.
- App order: saved address answers `/whoami`? use it. Otherwise listen for the beacon and, in parallel, probe the
  phone's own /24 with `/whoami`; first hit wins and is saved as the new address.

## PIN challenge-response
1. `GET /challenge` -> `200 {"nonce": "<hex string>"}`. `nonce` is random, single-use, short-lived (suggest 30 s
   server-side expiry).
2. Response = `HMAC-SHA256(key = SHA256(pin_utf8_bytes), message = nonce_hex_string_as_utf8_bytes)`, lowercase
   hex. **The message is the nonce's hex TEXT, not the decoded bytes** - confirmed from
   `crypto_service.dart::computePinResponse`. The firmware must HMAC over the same ASCII hex string it sent, not
   over `hex_decode(nonce)`.
3. `POST /auth` body `{"nonce": "...", "response": "..."}`.
   - `200 {"session_token": "...", "expires_in": <seconds, default assumed 120>}`
   - `401 {"error": "invalid_pin", "attempts_remaining": N}` - wrong PIN, attempts left.
   - `401 {"error": "locked_out", "attempts_remaining": 0}` - lockout engaged.
   - Lockout counters and duration are independent of the auth_fsm's own fingerprint+face lockout (Section
     2.5 of the Rev 2 doc: override/recovery keep their own PIN counters).

### Errors the app must handle (added with the PIN stage)
- `GET /challenge` and `POST /auth` -> `409 {"error":"no_pin_set"}` when no PIN exists yet (first-run: set one).
- `POST /auth` -> `400 {"error":"bad_nonce"}` for an unknown / expired (30 s) / already used nonce - not counted as a
  PIN attempt; just fetch a new challenge. PIN attempts: 5 wrong -> cooldown 30 s, 60 s, then 5 min
  (`401 locked_out` with `retry_after_ms`). Session tokens live 120 s.

## PIN management
- `GET /pin/status` -> `{"pin_set":bool,"locked_ms":N}` (no auth).
- `POST /pin/setup` body `{"pin_hash":"<64 hex = SHA256(pin_utf8)>"}`. Only while no PIN is set. The request blocks
  up to ~20 s while the lock waits for an **enrolled fingerprint** on its sensor (green LED blinks).
  `200 {"status":"pin_set"}`; `408 no_finger`; `403 fingerprint_mismatch`; `409 sensor_busy | pin_already_set`;
  `429 too_soon` (10 s between windows); `400 bad_pin_hash`.
- `POST /pin/change?token=<session_token>` body `{"new_pin_hash":"<64 hex>"}`. Needs a live session (old PIN proven)
  AND an enrolled fingerprint, same waiting rules. `200 {"status":"pin_changed"}` and the session ends (log in again
  with the new PIN); `401 session_expired`.
- Forgotten PIN: serial console `P` twice within 5 s erases it (physical access), then run first-time setup again.
- Known limit (prototype, plain HTTP): `pin_hash` is the PIN-equivalent HMAC key, so a passive sniffer on the same
  Wi-Fi who sees the setup/change request learns it, and the challenge/response pair lets them brute-force a short
  numeric PIN offline. The fingerprint at the door is what still protects the lock. Out of scope per Rev 2 2.6.

## Override
- `POST /override?token=<session_token>`
  - `200 {"status": "awaiting_fingerprint", "timeout_ms": 90000}` - opens a 90 s supervised window; any
    enrolled fingerprint matching within it unlocks (Rev 2 doc Section 1.2).
  - `401 {"error":"session_expired"}` - session token expired or invalid; app sends the user back to the PIN screen.
  - **(added)** `409 {"error":"sensor_busy"}` (a door session or another sensor operation is running) or
    `{"error":"override_active"}` (a window is already open). The session token is consumed by a successful call.
  - The face stage is skipped (that is the point of override). Once users exist, only a finger that belongs to one of
    them opens the door. A wrong finger does not end the window; three do.

## Session WebSocket (enroll / manage)
`GET /session?token=<session_token>` upgrades to WS. Every use of this endpoint starts the same way:

0. **(added)** The token must be valid at connect time and is consumed. A bad token or a second concurrent session
   gets `{"type":"state","state":"unauthorized"}` / `{"type":"state","state":"busy"}` and the socket is closed.
1. Server -> `{"type":"state","state":"awaiting_fingerprint"}` on connect.
2. Owner touches the sensor. Server -> `{"type":"state","state":"fingerprint_confirmed"}` on match, or
   `{"type":"state","state":"fingerprint_failed"}` / `{"type":"state","state":"timeout"}` on failure (either
   closes the session).
3. Client picks a mode: `{"type":"select_mode","mode":"enroll","name":"<string>"}` or
   `{"type":"select_mode","mode":"manage"}`.

### Enroll mode
- Server streams live JPEG frames as raw WS **binary** messages (no envelope) at a steady rate.
- Server also sends, once per processed frame:
  `{"type":"detection","face_found":bool,"bbox":{"x":N,"y":N,"w":N,"h":N},"frame_w":240,"frame_h":240}`
  `bbox`/`frame_w`/`frame_h` are omitted when `face_found` is false. **`frame_w`/`frame_h` must always be sent**
  - the app was found hardcoding 96x96 (the actual detector frame is 240x240, see `app_main.cpp`); sending the
    real size on every message is what makes that impossible to drift out of sync again.
- Client -> `{"type":"capture"}` when it wants the current frame stored as one of the person's samples. Only
  meaningful while `face_found` is true.
- Server -> `{"type":"capture_result","success":bool,"frame_index":N,"frames_needed":5,"reason":"no_face_detected"|"capture_failed"}`
  (`reason` only on failure). **Five samples per person** (`frames_needed` is fixed at 5 - see the multi-sample
  enrollment note below), one `capture` per sample, guided by the app's "turn your head slightly" hint between
  captures.
- **(added)** The new person's fingerprint is part of enrollment (this is what binds finger to face). After the 5th
  successful capture: server -> `{"type":"state","state":"awaiting_new_fingerprint"}`, then
  `{"type":"fingerprint_progress","step":"place1"|"remove"|"place2"|"storing"}` as the sensor asks for the same finger
  twice (15 s per step).
- Then server -> `{"type":"enrollment_complete","user_id":"<id>"}` and closes.
- **(added)** Any failure after captures started -> `{"type":"enrollment_failed","reason":"..."}` with
  `bad_name | user_table_full | sensor_busy | fingerprint_timeout | fingerprint_mismatch | fingerprint_library_full |
  sensor_error | storage`; every face captured so far is deleted again, nothing half-enrolled is kept.
- Preview frames are baseline JPEG (240x240, ~4-5 per second), each followed by its `detection` message.

### Manage mode
- Server -> `{"type":"user_list","users":[{"id":"...","name":"..."}]}` on entering manage mode.
- Client -> `{"type":"delete_user","id":"..."}` to delete a person entirely.
- Server -> `{"type":"delete_result","success":bool,"id":"..."}`.
- Client -> `{"type":"done"}` when finished (sent from `dispose()`); server closes the socket.
- **(added)** Deleting removes the fingerprint template first, then every face, then the record. If the template cannot
  be deleted the record is kept and `delete_result` says `success:false` so the owner can retry.
- **(added)** If the last user is deleted the table is empty again and the door falls back to the permissive
  pre-owner-app behaviour (any enrolled finger + any enrolled face), because nothing records who owns what.

## User model (this is the fix for the fingerprint/face binding gap)
A "user" is one record, not two independent enrollments:
```
{ id, name, fingerprint_slot: uint16, face_feature_ids: [uint16 x up to 5] }
```
`id` is assigned by the device and is what `manage`'s `user_list`/`delete_user` operate on - it is NOT the same
number as `fingerprint_slot` or any single `face_feature_ids` entry (those are AS608/esp-dl's own internal
numbering). Deleting a user removes the AS608 template AND every one of that user's face features, so a stale
fingerprint or a stale face can't survive a partial delete.

### Binding rule at the door (added)
`auth_task` now checks the matched face against the matched finger: the face id must be one of the faces of the user
that owns that fingerprint slot (`user_db_authorize`). **With no users recorded the check is off** (boards enrolled
only through the console keep working); after the first app enrollment it is on, so console-enrolled fingers/faces that
are not part of a user stop opening the door. Start clean with `X` (erase fingerprints) + `d` (delete faces) if you want
a fresh table. The owner session itself also requires a finger that belongs to a user once users exist.

## Multi-sample face enrollment
5 captures per person, stored as 5 separate feature vectors under that person's record (esp-dl's
`HumanFaceRecognizer` has no "average multiple samples into one template" API - it only supports one vector per
`enroll()` call - so 5 samples means 5 stored features, matched independently; a match against any one of the 5
counts as a match for that person). This raises FAR slightly (5 chances at the threshold instead of 1) -
measure this in Phase 7 rather than assuming it's negligible.
