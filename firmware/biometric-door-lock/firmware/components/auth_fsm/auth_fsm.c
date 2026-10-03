#include "auth_fsm.h"

void auth_fsm_init(auth_fsm_t *fsm)
{
    fsm->state = AUTH_STATE_IDLE;
}

const char *auth_fsm_state_name(auth_fsm_state_t s)
{
    return (s == AUTH_STATE_IDLE) ? "idle" : "await_face";
}

auth_fsm_action_t auth_fsm_on_event(auth_fsm_t *fsm, auth_fsm_event_t ev)
{
    switch (fsm->state) {
    case AUTH_STATE_IDLE:
        switch (ev) {
        case AUTH_EV_FINGER_PASS:
            fsm->state = AUTH_STATE_AWAIT_FACE;
            return AUTH_ACT_START_FACE_CHECK;
        case AUTH_EV_FINGER_FAIL:
            return AUTH_ACT_DENY; /* stays IDLE */
        default:
            return AUTH_ACT_NONE; /* a face result with no fingerprint pass first: ignored, not an unlock */
        }
    case AUTH_STATE_AWAIT_FACE:
        switch (ev) {
        case AUTH_EV_FACE_PASS:
            fsm->state = AUTH_STATE_IDLE;
            return AUTH_ACT_UNLOCK;
        case AUTH_EV_FACE_FAIL:
            fsm->state = AUTH_STATE_IDLE;
            return AUTH_ACT_DENY;
        default:
            return AUTH_ACT_NONE; /* fingerprint is not consulted again once face check has started */
        }
    }
    return AUTH_ACT_NONE;
}
