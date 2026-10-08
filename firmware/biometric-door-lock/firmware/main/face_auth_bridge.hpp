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

// ---- owner-app enrollment support (session_ws.cpp). Everything below and face_identify_blocking() share one mutex, so
// a door session and an enrollment/deletion can never touch the recognition task or the face database together.

// Hands over the recognizer created in app_main.cpp (needed to delete a feature by id) and creates the mutex.
// Call once, before any task can use the functions here.
void face_auth_init(HumanFaceRecognizer *recognizer);

// Enrolls the face in the next camera frame as one new feature. On success *out_id is the id esp-dl actually assigned
// (read back from the database header: the "id: N enrolled." text is wrong once any feature has been deleted).
// False = no face in that frame, enroll failed, or timeout.
bool face_enroll_blocking(who::recognition::WhoRecognitionCore *task, uint32_t timeout_ms, uint16_t *out_id);

// Deletes one feature by id. False if the database refused it (unknown id).
bool face_delete(uint16_t id);

// The recognizer's "id" for a match is the 1-based POSITION among the stored faces, not the id esp-dl stored (they drift
// apart after any deletion). The user table and face_delete() use stored ids, so translate before comparing. -1 = unknown.
int face_stored_id_for_position(int position);

// Deletes every stored face for which keep(id) is false (stored ids). keep == nullptr wipes the whole database and
// starts a fresh file, so ids restart at 1 and recognizer positions equal stored ids again. Returns how many faces were
// deleted, or -1 on failure; *kept (optional) receives how many remain.
int face_prune(bool (*keep)(uint16_t id), int *kept);

