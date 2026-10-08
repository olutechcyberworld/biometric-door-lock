#pragma once
// POST /override?token=<session_token>  (docs/APP_PROTOCOL.md "Override"): PIN already proven -> a 90 s supervised window
// in which an enrolled fingerprint opens the door WITHOUT the face stage (the owner is at the door but a factor is failing).
void override_api_start();  // registers the handler with device_link
bool override_api_active(); // true while an override window is open (the door button must not compete for the sensor)
