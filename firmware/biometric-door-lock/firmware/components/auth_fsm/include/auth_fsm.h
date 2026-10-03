#pragma once
/*
 * Core authentication state machine (Phase 3): fingerprint gates face recognition, sequentially, with no stage
 * skippable. Pure logic, no ESP-IDF/hardware dependency, so it runs the same on the board and in host tests
 * (see test/test_auth_fsm.cpp) - the same split the AS608 driver uses (as608.c vs as608_uart.c).
 *
 * Usage: call auth_fsm_on_event() once per fingerprint/face outcome and act on the returned action. The caller
 * (main/auth_task.cpp) owns all I/O - this file only decides what should happen next.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUTH_EV_FINGER_PASS,
    AUTH_EV_FINGER_FAIL, /* a finger was read and is not enrolled. Timeouts/sensor errors are not attempts: the
                            caller (main/auth_task.cpp) filters them out before they reach the FSM. */
    AUTH_EV_FACE_PASS,
    AUTH_EV_FACE_FAIL,
} auth_fsm_event_t;

typedef enum {
    AUTH_ACT_NONE,           /* event ignored in this state (e.g. a face result with no fingerprint pass yet) */
    AUTH_ACT_START_FACE_CHECK,
    AUTH_ACT_UNLOCK,
    AUTH_ACT_DENY,
} auth_fsm_action_t;

typedef enum {
    AUTH_STATE_IDLE,       /* waiting for a fingerprint */
    AUTH_STATE_AWAIT_FACE, /* fingerprint passed; waiting for the face check */
} auth_fsm_state_t;

typedef struct {
    auth_fsm_state_t state;
} auth_fsm_t;

void auth_fsm_init(auth_fsm_t *fsm);
auth_fsm_action_t auth_fsm_on_event(auth_fsm_t *fsm, auth_fsm_event_t ev);
const char *auth_fsm_state_name(auth_fsm_state_t s);

#ifdef __cplusplus
}
#endif
