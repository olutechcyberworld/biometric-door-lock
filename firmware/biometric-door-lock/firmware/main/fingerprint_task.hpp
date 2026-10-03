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
