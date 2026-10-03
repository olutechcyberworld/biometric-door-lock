# App <-> Firmware Protocol (the "Firmware Contract" the app was built against)

Status: **reverse-engineered from `biometric-doorlock-app` on 2026-09-28.** No written Sections 4/5 existed in
either repo before this file - the app code was the only spec. This document is now the single source of truth
for both sides; when they disagree, fix the code to match this file, not the other way round.

Not implemented in firmware yet: everything below is what the app already assumes. Wi-Fi, the HTTP server and
the WebSocket session are the next patch (see `overview.md` Phase 4/5 status) - this file is written now so that
patch has a fixed target instead of being reverse-engineered a second time.

## Transport
- Plain HTTP + WS, local Wi-Fi only, no TLS - matches Rev 2 doc Section 2.6 (TLS explicitly out of scope for the
  prototype).
- Device advertises as `doorlock.local` (mDNS) - the app's default host. If mDNS resolution isn't reliable on
  your router, the app also accepts a raw IP entered in Settings.

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

## Override
- `POST /override?token=<session_token>`
  - `200 {"status": "awaiting_fingerprint", "timeout_ms": 90000}` - opens a 90 s supervised window; any
    enrolled fingerprint matching within it unlocks (Rev 2 doc Section 1.2).
  - `401` - session token expired or invalid; app sends the user back to the PIN screen.

## Session WebSocket (enroll / manage)
`GET /session?token=<session_token>` upgrades to WS. Every use of this endpoint starts the same way:

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
- After the 5th successful capture: server -> `{"type":"enrollment_complete"}` and closes.

### Manage mode
- Server -> `{"type":"user_list","users":[{"id":"...","name":"..."}]}` on entering manage mode.
- Client -> `{"type":"delete_user","id":"..."}` to delete a person entirely.
- Server -> `{"type":"delete_result","success":bool,"id":"..."}`.
- Client -> `{"type":"done"}` when finished (sent from `dispose()`); server closes the socket.

## User model (this is the fix for the fingerprint/face binding gap)
A "user" is one record, not two independent enrollments:
```
{ id, name, fingerprint_slot: uint16, face_feature_ids: [uint16 x up to 5] }
```
`id` is assigned by the device and is what `manage`'s `user_list`/`delete_user` operate on - it is NOT the same
number as `fingerprint_slot` or any single `face_feature_ids` entry (those are AS608/esp-dl's own internal
numbering). Deleting a user removes the AS608 template AND every one of that user's face features, so a stale
fingerprint or a stale face can't survive a partial delete.

## Multi-sample face enrollment
5 captures per person, stored as 5 separate feature vectors under that person's record (esp-dl's
`HumanFaceRecognizer` has no "average multiple samples into one template" API - it only supports one vector per
`enroll()` call - so 5 samples means 5 stored features, matched independently; a match against any one of the 5
counts as a match for that person). This raises FAR slightly (5 chances at the threshold instead of 1) -
measure this in Phase 7 rather than assuming it's negligible.
