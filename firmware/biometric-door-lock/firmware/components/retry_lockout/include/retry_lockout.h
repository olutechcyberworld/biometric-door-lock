#pragma once
#include <stdbool.h>
#include <stdint.h>
/*
 * Failed-attempt counter and escalating cooldown for the auth loop (Phase 4). Pure logic, no ESP-IDF - same
 * split as auth_fsm and as608.c: the caller (main/auth_task.cpp) owns the clock and the LED/buzzer, this file
 * only decides retries-left / lockout-or-not / how long.
 *
 * Cooldown schedule escalates each time a NEW lockout is entered (30s, then 1 min, then 5 min, capped there),
 * and resets to the first step only after a real unlock - not just after the cooldown expires - so repeated
 * failure/wait/failure cycles keep getting longer instead of resetting for free every time the clock runs out.
 */
#ifdef __cplusplus
extern "C" {
#endif

#define RETRY_LOCKOUT_MAX_STEPS 3

typedef struct {
    uint8_t max_attempts;                                 /* failures allowed before a lockout starts */
    uint32_t cooldown_ms[RETRY_LOCKOUT_MAX_STEPS];         /* escalation schedule, e.g. {30000, 60000, 300000} */
} retry_lockout_cfg_t;

typedef struct {
    retry_lockout_cfg_t cfg;
    uint8_t fail_count;      /* consecutive failures since the last granted unlock */
    uint8_t cooldown_step;   /* how far into cfg.cooldown_ms[] the NEXT lockout will start (caps at the last) */
    bool locked;
    uint32_t unlock_at_ms;   /* only meaningful while locked */
} retry_lockout_t;

typedef enum {
    RETRY_ACT_NONE,      /* attempt failed but retries remain */
    RETRY_ACT_LOCKOUT,   /* this failure just triggered a new lockout; unlock_at_ms is now set */
    RETRY_ACT_IGNORED,   /* still locked out - caller should not even have attempted; nothing changed */
} retry_lockout_action_t;

void retry_lockout_init(retry_lockout_t *r, retry_lockout_cfg_t cfg);

/* True if the caller should refuse to even try an attempt right now. now_ms must be non-decreasing between
 * calls (e.g. esp_timer_get_time()/1000 or xTaskGetTickCount()*portTICK_PERIOD_MS). Clears `locked` itself the
 * first time it's asked after the cooldown has elapsed, so the caller never has to poll a separate timer. */
bool retry_lockout_is_locked(retry_lockout_t *r, uint32_t now_ms);

/* Call on every granted unlock: clears the failure count AND resets the cooldown schedule back to step 0. */
void retry_lockout_on_success(retry_lockout_t *r);

/* Call on every failed attempt (fingerprint or face - either counts, per main/auth_task.cpp). Returns
 * RETRY_ACT_IGNORED without changing state if already locked out (should not normally happen: check
 * retry_lockout_is_locked() first and skip the attempt entirely). */
retry_lockout_action_t retry_lockout_on_fail(retry_lockout_t *r, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
