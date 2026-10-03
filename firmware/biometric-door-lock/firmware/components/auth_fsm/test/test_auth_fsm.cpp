// Host test for the core authentication FSM (Phase 3). No ESP-IDF, no board - build/run per tests/run_host_tests.sh.
#include <cassert>
#include <cstdio>
#include "auth_fsm.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    // Normal grant: finger pass -> face pass -> unlock, and the fsm returns to idle ready for the next person.
    {
        auth_fsm_t f; auth_fsm_init(&f);
        CHECK(f.state == AUTH_STATE_IDLE);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FINGER_PASS) == AUTH_ACT_START_FACE_CHECK);
        CHECK(f.state == AUTH_STATE_AWAIT_FACE);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FACE_PASS) == AUTH_ACT_UNLOCK);
        CHECK(f.state == AUTH_STATE_IDLE);
    }
    // Fingerprint fails: denied immediately, face is never asked.
    {
        auth_fsm_t f; auth_fsm_init(&f);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FINGER_FAIL) == AUTH_ACT_DENY);
        CHECK(f.state == AUTH_STATE_IDLE);
    }
    // Fingerprint passes, face fails: denied, back to idle (not stuck waiting for face).
    {
        auth_fsm_t f; auth_fsm_init(&f);
        auth_fsm_on_event(&f, AUTH_EV_FINGER_PASS);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FACE_FAIL) == AUTH_ACT_DENY);
        CHECK(f.state == AUTH_STATE_IDLE);
    }
    // No factor skippable: a face event with no prior fingerprint pass grants nothing.
    {
        auth_fsm_t f; auth_fsm_init(&f);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FACE_PASS) == AUTH_ACT_NONE);
        CHECK(f.state == AUTH_STATE_IDLE);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FACE_FAIL) == AUTH_ACT_NONE);
        CHECK(f.state == AUTH_STATE_IDLE);
    }
    // Once face-checking has started, a stray fingerprint result (e.g. a leftover auto-retry) is not consulted again.
    {
        auth_fsm_t f; auth_fsm_init(&f);
        auth_fsm_on_event(&f, AUTH_EV_FINGER_PASS);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FINGER_PASS) == AUTH_ACT_NONE);
        CHECK(f.state == AUTH_STATE_AWAIT_FACE);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FINGER_FAIL) == AUTH_ACT_NONE);
        CHECK(f.state == AUTH_STATE_AWAIT_FACE);
    }
    // Two people in a row: a deny must not leak state into the next attempt.
    {
        auth_fsm_t f; auth_fsm_init(&f);
        auth_fsm_on_event(&f, AUTH_EV_FINGER_FAIL);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FINGER_PASS) == AUTH_ACT_START_FACE_CHECK);
        CHECK(auth_fsm_on_event(&f, AUTH_EV_FACE_PASS) == AUTH_ACT_UNLOCK);
    }
    printf("ALL PASSED\n");
    return 0;
}
