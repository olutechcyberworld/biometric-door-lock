#pragma once
#include <cstdint>
// Fingerprint interface (Phase 2). One FreeRTOS task on core 0 owns the AS608; everything else talks to it through
// fingerprint_post(). Console / dashboard commands:
//   f = identify (wait for a finger, 1:N search on the module)   n = enroll into the next free slot
//   p = probe the sensor again                                    X = erase the whole library (send twice within 5 s)
// Output lines (machine readable, consumed by tools/dashboard.py):
//   FPINIT,result=ok|fail,...          FPSTATE,count=N,capacity=M
//   FP,identify,step=place | result=match|nomatch|timeout|error,...
//   FP,enroll,step=start|place1|remove|place2|storing | result=ok|fail,...     FP,empty,result=...
void fingerprint_start();
bool fingerprint_post(char cmd);

// Phase 3: one blocking identify pass for main/auth_task.cpp, serialized through the SAME fingerprint_task thread
// as every console/dashboard command (nothing here ever touches the AS608 from a second thread). Returns false
// immediately, doing nothing, if the sensor is already busy with a console/dashboard command - the auth loop just
// retries shortly after; it never queues up behind a manual command.
enum fingerprint_auth_outcome_t : uint8_t {
    FP_AUTH_MATCH,   // enrolled finger recognised
    FP_AUTH_NOMATCH, // a finger was read but is not enrolled
    FP_AUTH_TIMEOUT, // nobody touched the sensor within the wait window: not an attempt at all
    FP_AUTH_ERROR,   // sensor/link error
};
struct fingerprint_auth_result_t {
    fingerprint_auth_outcome_t outcome;
    uint16_t id;
    uint16_t score;
};
bool fingerprint_post_auth_identify(uint32_t timeout_ms);
bool fingerprint_get_auth_result(fingerprint_auth_result_t *out, uint32_t wait_ms);

// Owner-app enrollment (session_ws.cpp): enroll a NEW finger into the next free slot, and delete a template by slot.
// Same rules as the auth identify above: serialized through the fingerprint task, refused (false) while the sensor
// is busy, never queued. Progress is reported on the fingerprint task - keep the callback short.
enum fingerprint_enroll_outcome_t : uint8_t {
    FP_ENROLL_OK,
    FP_ENROLL_TIMEOUT, // the finger was not placed / removed in time
    FP_ENROLL_NOMATCH, // the two scans did not agree, or the image was too poor: try again
    FP_ENROLL_FULL,    // sensor library is full
    FP_ENROLL_ERROR,   // sensor/link error
};
struct fingerprint_enroll_result_t {
    fingerprint_enroll_outcome_t outcome;
    uint16_t id; // the slot that was written (valid when outcome == FP_ENROLL_OK)
};
typedef void (*fingerprint_enroll_progress_cb)(int step, void *user); // 0 place1, 1 remove, 2 place2, 3 storing
bool fingerprint_post_enroll(uint32_t step_timeout_ms, fingerprint_enroll_progress_cb cb, void *user);
bool fingerprint_get_enroll_result(fingerprint_enroll_result_t *out, uint32_t wait_ms);
bool fingerprint_delete_slot(uint16_t id, uint32_t wait_ms); // blocking; true only if the module confirmed the delete

// Delete every stored fingerprint template for which keep(id) is false. Used once users exist, to clear console-enrolled
// templates that belong to nobody (they can shadow a user's template of the same finger). False = sensor busy.
typedef bool (*fingerprint_keep_fn)(uint16_t id);
bool fingerprint_prune(fingerprint_keep_fn keep);

