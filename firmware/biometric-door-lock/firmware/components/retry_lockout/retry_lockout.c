#include "retry_lockout.h"

void retry_lockout_init(retry_lockout_t *r, retry_lockout_cfg_t cfg)
{
    r->cfg = cfg;
    r->fail_count = 0;
    r->cooldown_step = 0;
    r->locked = false;
    r->unlock_at_ms = 0;
}

bool retry_lockout_is_locked(retry_lockout_t *r, uint32_t now_ms)
{
    if (r->locked && (int32_t)(now_ms - r->unlock_at_ms) >= 0) {
        r->locked = false;
        r->fail_count = 0; /* cooldown served: give a fresh set of attempts at the CURRENT (not reset) step */
    }
    return r->locked;
}

void retry_lockout_on_success(retry_lockout_t *r)
{
    r->fail_count = 0;
    r->cooldown_step = 0;
    r->locked = false;
}

retry_lockout_action_t retry_lockout_on_fail(retry_lockout_t *r, uint32_t now_ms)
{
    if (r->locked) {
        return RETRY_ACT_IGNORED;
    }
    r->fail_count++;
    if (r->fail_count < r->cfg.max_attempts) {
        return RETRY_ACT_NONE;
    }
    uint8_t step = r->cooldown_step;
    if (step >= RETRY_LOCKOUT_MAX_STEPS) {
        step = RETRY_LOCKOUT_MAX_STEPS - 1;
    }
    r->locked = true;
    r->unlock_at_ms = now_ms + r->cfg.cooldown_ms[step];
    if (r->cooldown_step < RETRY_LOCKOUT_MAX_STEPS - 1) {
        r->cooldown_step++; /* next lockout (if the fresh attempts after this one also run out) escalates */
    }
    return RETRY_ACT_LOCKOUT;
}
