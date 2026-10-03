// Host test for retry_lockout (Phase 4). No ESP-IDF - build/run per tests/run_host_tests.sh.
#include <cstdio>
#include "retry_lockout.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static retry_lockout_cfg_t cfg()
{
    retry_lockout_cfg_t c = {};
    c.max_attempts = 3;
    c.cooldown_ms[0] = 30000;
    c.cooldown_ms[1] = 60000;
    c.cooldown_ms[2] = 300000;
    return c;
}

int main()
{
    // Under the limit: no lockout, caller never blocked.
    {
        retry_lockout_t r; retry_lockout_init(&r, cfg());
        CHECK(!retry_lockout_is_locked(&r, 0));
        CHECK(retry_lockout_on_fail(&r, 1000) == RETRY_ACT_NONE);
        CHECK(retry_lockout_on_fail(&r, 2000) == RETRY_ACT_NONE);
        CHECK(!retry_lockout_is_locked(&r, 2000));
    }
    // 3rd consecutive failure triggers a lockout for the first (shortest) step.
    {
        retry_lockout_t r; retry_lockout_init(&r, cfg());
        retry_lockout_on_fail(&r, 0);
        retry_lockout_on_fail(&r, 100);
        CHECK(retry_lockout_on_fail(&r, 200) == RETRY_ACT_LOCKOUT);
        CHECK(retry_lockout_is_locked(&r, 200));
        CHECK(retry_lockout_is_locked(&r, 200 + 29999));
        CHECK(!retry_lockout_is_locked(&r, 200 + 30000)); // cooldown served, exactly at the boundary
    }
    // A failed attempt while already locked out changes nothing and is reported as ignored.
    {
        retry_lockout_t r; retry_lockout_init(&r, cfg());
        retry_lockout_on_fail(&r, 0); retry_lockout_on_fail(&r, 0); retry_lockout_on_fail(&r, 0);
        uint32_t unlock_at = r.unlock_at_ms;
        CHECK(retry_lockout_on_fail(&r, 500) == RETRY_ACT_IGNORED);
        CHECK(r.unlock_at_ms == unlock_at); // not extended by hammering it while locked
    }
    // A real unlock resets both the count AND the escalation schedule.
    {
        retry_lockout_t r; retry_lockout_init(&r, cfg());
        retry_lockout_on_fail(&r, 0); retry_lockout_on_fail(&r, 0); retry_lockout_on_fail(&r, 0);
        CHECK(retry_lockout_is_locked(&r, 29999)); // still within the 30s window
        retry_lockout_on_success(&r); // e.g. the owner used override instead of waiting it out
        CHECK(!retry_lockout_is_locked(&r, 30000));
        retry_lockout_on_fail(&r, 30000); retry_lockout_on_fail(&r, 30000);
        CHECK(retry_lockout_on_fail(&r, 30000) == RETRY_ACT_LOCKOUT);
        CHECK(r.unlock_at_ms == 30000 + 30000); // back to step 0's 30s, not escalated by the earlier lockout
    }
    // Failing again right after a cooldown expires escalates to the next (longer) step.
    {
        retry_lockout_t r; retry_lockout_init(&r, cfg());
        retry_lockout_on_fail(&r, 0); retry_lockout_on_fail(&r, 0); retry_lockout_on_fail(&r, 0);
        CHECK(!retry_lockout_is_locked(&r, 30000)); // cooldown 1 served
        retry_lockout_on_fail(&r, 30000); retry_lockout_on_fail(&r, 30000);
        CHECK(retry_lockout_on_fail(&r, 30000) == RETRY_ACT_LOCKOUT);
        CHECK(r.unlock_at_ms == 30000 + 60000); // step 1 now, not another 30s
        CHECK(!retry_lockout_is_locked(&r, 90000)); // cooldown 2 served
        retry_lockout_on_fail(&r, 90000); retry_lockout_on_fail(&r, 90000);
        CHECK(retry_lockout_on_fail(&r, 90000) == RETRY_ACT_LOCKOUT);
        CHECK(r.unlock_at_ms == 90000 + 300000); // step 2 (5 min)
    }
    // Escalation caps at the last configured step (5 min) and does not run past the array, however many
    // lockout/wait-it-out cycles happen in a row without ever calling retry_lockout_on_success().
    {
        retry_lockout_t r; retry_lockout_init(&r, cfg());
        uint32_t t = 0;
        const uint32_t expected[] = {30000, 60000, 300000, 300000, 300000}; // step index caps at cooldown_ms[2]
        for (int cycle = 0; cycle < 5; cycle++) {
            retry_lockout_on_fail(&r, t); retry_lockout_on_fail(&r, t);
            retry_lockout_on_fail(&r, t);
            CHECK(r.unlock_at_ms == t + expected[cycle]);
            CHECK(retry_lockout_is_locked(&r, t + expected[cycle] - 1));
            t += expected[cycle];
            CHECK(!retry_lockout_is_locked(&r, t)); // cooldown served, waited out rather than reset by success
        }
    }
    printf("ALL PASSED\n");
    return 0;
}
