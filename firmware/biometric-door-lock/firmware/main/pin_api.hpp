#pragma once
// Owner-app PIN endpoints (docs/APP_PROTOCOL.md): GET /challenge, POST /auth, GET /pin/status, POST /pin/setup,
// POST /pin/change. Logic lives in components/pin_auth (host-tested); this file is storage + HTTP + the fingerprint gate.
void pin_api_start();         // after wifi_manager_start() (needs NVS); registers handlers with device_link
bool pin_api_console_reset(); // serial 'P' twice within 5 s: erases the PIN. Returns true when it actually erased.
bool pin_api_session_valid(const char *token); // token from POST /auth, still within its 120 s
void pin_api_session_end();                    // single-use hygiene: override and /session end it once they have accepted it

