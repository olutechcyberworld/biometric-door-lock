#pragma once
// GET /session?token=<session_token>  (docs/APP_PROTOCOL.md "Session WebSocket"): owner confirms with a finger, then
// either enrolls a new person (live preview, 5 face captures, new finger) or manages (list / delete) users.
#include "who_recognition.hpp"

void session_ws_start(who::recognition::WhoRecognitionCore *recog); // registers the handler + the frame tap
bool session_ws_active();                                           // true while a session owns the camera/sensor
