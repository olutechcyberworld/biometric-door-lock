#pragma once
// Phase 3: bridges ESP-WHO's async RECOGNIZE event (fires on whatever frame next has a face - no timeout of its
// own) into a blocking call with a timeout, for main/auth_task.cpp. Only one caller should be waiting at a time.
#include <cstddef>
#include <cstdint>
#include <string>
#include "who_recognition.hpp"

// Called from the detect_result_cb (set in app_main.cpp) on every recognition frame, just before the result callback:
// records how many faces the detector saw in that frame.
void face_auth_on_detect(size_t faces);

// Called from the recognition_result_cb set in app_main.cpp - any task context. Ignored unless a
// face_identify_blocking() call is currently waiting, so a manual dashboard 'r' press outside the auth loop
// (or a face that shows up after our own timeout already gave up) can't be mistaken for an auth-loop result.
void face_auth_on_result(const std::string &result);

// Waits up to timeout_ms for the owner's face. ESP-WHO's RECOGNIZE handles exactly ONE frame whether or not a face is
// in it, so this re-requests while the frame had no face at all (the person has not stepped into view yet). A frame
// that DID contain a face which is not recognised ends the attempt immediately with false - it is never retried, so
// waiting for a face cannot turn into repeated tries at the match threshold (that would inflate the FAR). Returns
// false on that, or on timeout. A result that arrives after we give up is dropped, not applied late.
bool face_identify_blocking(who::recognition::WhoRecognitionCore *task, uint32_t timeout_ms, int *out_id,
                             float *out_sim);
