#include "auth_task.hpp"

#include "sdkconfig.h"

#if CONFIG_DOORLOCK_AUTH_AUTO

#include <cstdio>
#include "auth_fsm.h"
#include "esp_timer.h"
#include "face_auth_bridge.hpp"
#include "fingerprint_task.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "relay_control.h"
#include "retry_lockout.h"
#include "who_recognition.hpp"
#include "wifi_manager.h"
#include "override_api.hpp"
#include "session_ws.hpp"
#include "user_db.hpp"

using who::recognition::WhoRecognitionCore;

namespace {

uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Blocks until a session should start: not in a lockout cooldown, AND the button has been pressed (debounced).
// Owns the lockout LED while idle, since this is the only place the device sits and waits.
void wait_for_session_start(retry_lockout_t *lockout, bool *lockout_led_on)
{
    for (;;) {
        if (retry_lockout_is_locked(lockout, now_ms())) {
            if (!*lockout_led_on) {
                relay_control_lockout_set(true);
                *lockout_led_on = true;
            }
            vTaskDelay(pdMS_TO_TICKS(200)); // poll for the cooldown expiring; button is ignored while locked out
            continue;
        }
        if (*lockout_led_on) {
            relay_control_lockout_set(false);
            *lockout_led_on = false;
        }
        if (!relay_control_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(30)); // debounce: require the press to still read low after 30 ms
        if (relay_control_button_pressed()) {
            // Confirmed press. Short press -> session (starts on RELEASE, so a long hold can't also start one).
            // Held for CONFIG_DOORLOCK_PROV_HOLD_MS -> one beep + Wi-Fi provisioning mode, no session.
            uint32_t t0 = now_ms();
            bool long_fired = false;
            while (relay_control_button_pressed()) {
                if (!long_fired && now_ms() - t0 >= CONFIG_DOORLOCK_PROV_HOLD_MS) {
                    long_fired = true;
                    printf("AUTH,button=long_hold\n");
                    relay_control_beep(150);
                    wifi_manager_enter_provisioning(); // opens the setup network; returns once it is up
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (long_fired) {
                continue; // the hold was for provisioning, not a door session
            }
            return; // released before the hold time - caller starts a session
        }
        // was noise, not a real press - fall through and keep waiting
    }
}

void auth_task(void *arg)
{
    auto *recog = static_cast<WhoRecognitionCore *>(arg);
    auth_fsm_t fsm;
    auth_fsm_init(&fsm);

    retry_lockout_t lockout;
    retry_lockout_cfg_t lockout_cfg = {};
    lockout_cfg.max_attempts = CONFIG_DOORLOCK_MAX_ATTEMPTS;
    lockout_cfg.cooldown_ms[0] = CONFIG_DOORLOCK_COOLDOWN_1_MS;
    lockout_cfg.cooldown_ms[1] = CONFIG_DOORLOCK_COOLDOWN_2_MS;
    lockout_cfg.cooldown_ms[2] = CONFIG_DOORLOCK_COOLDOWN_3_MS;
    retry_lockout_init(&lockout, lockout_cfg);
    bool lockout_led_on = false; // mirrors retry_lockout_t's own `locked`, so we set the red LED only on change

    for (;;) {
        wait_for_session_start(&lockout, &lockout_led_on);
        if (session_ws_active() || override_api_active()) {
            // an owner session / override window owns the sensor and the camera: this press is simply not served
            printf("AUTH,session=ignored,reason=owner_session_active\n");
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        printf("AUTH,session=start\n");

        // Ask fingerprint_task for one identify pass. It refuses (returns false) if a manual console/dashboard
        // command currently owns the sensor - the button press is simply lost; the owner just presses again.
        relay_control_green_blink_start(); // "place your finger" - stopped the instant this stage resolves
        if (!fingerprint_post_auth_identify(CONFIG_DOORLOCK_FINGER_WAIT_MS)) {
            relay_control_green_blink_stop();
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        fingerprint_auth_result_t fr = {};
        bool got_result = fingerprint_get_auth_result(&fr, CONFIG_DOORLOCK_FINGER_WAIT_MS + 1000);
        relay_control_green_blink_stop();
        if (!got_result) {
            continue; // shouldn't happen (identify_once always posts a result); don't get stuck if it ever does
        }

        if (fr.outcome == FP_AUTH_TIMEOUT) {
            printf("AUTH,session=end,reason=no_finger\n");
            continue; // nobody touched the sensor within the session window: not an attempt, session just ends
        }
        if (fr.outcome == FP_AUTH_ERROR) {
            printf("AUTH,result=error,stage=fingerprint\n");
            vTaskDelay(pdMS_TO_TICKS(500)); // sensor/link problem: report it and back off instead of spinning
            continue;
        }

        auth_fsm_action_t act = auth_fsm_on_event(&fsm, fr.outcome == FP_AUTH_MATCH ? AUTH_EV_FINGER_PASS
                                                                                     : AUTH_EV_FINGER_FAIL);
        if (act == AUTH_ACT_DENY) {
            printf("AUTH,result=deny,stage=fingerprint\n");
            if (retry_lockout_on_fail(&lockout, now_ms()) == RETRY_ACT_LOCKOUT) {
                printf("AUTH,lockout=enter,cooldown_ms=%u\n", (unsigned)(lockout.unlock_at_ms - now_ms()));
                relay_control_lockout_enter();
            } else {
                relay_control_deny();
            }
            continue;
        }
        if (act != AUTH_ACT_START_FACE_CHECK) {
            continue; // AUTH_ACT_NONE can't happen from AUTH_STATE_IDLE on a finger event; defensive only
        }

        printf("AUTH,stage=face,step=start\n");
        relay_control_white_led(true); // "you can look at the camera now" - also lights the face for Phase 1's
                                        // dim-light weak point
        int face_id = -1;
        float face_sim = 0.f;
        bool face_ok = face_identify_blocking(recog, 6000, &face_id, &face_sim);
        relay_control_white_led(false);
        // Binding: the recognised face must belong to the same user as the matched finger (user_db). With no users
        // recorded yet this stays permissive, as it was before the owner app existed.
        if (face_ok) {
            int stored_id = face_stored_id_for_position(face_id); // recognizer id = list position, not the stored id
            if (stored_id < 0 || !user_db_authorize(fr.id, (uint16_t)stored_id)) {
                printf("AUTH,binding=mismatch,fp_id=%u,face_pos=%d,face_id=%d\n", (unsigned)fr.id, face_id, stored_id);
                face_ok = false;
            }
        }
        act = auth_fsm_on_event(&fsm, face_ok ? AUTH_EV_FACE_PASS : AUTH_EV_FACE_FAIL);
        if (act == AUTH_ACT_UNLOCK) {
            printf("AUTH,result=grant,fp_id=%u,face_id=%d,face_sim=%.2f\n", (unsigned)fr.id, face_id, face_sim);
            retry_lockout_on_success(&lockout);
            relay_control_unlock(CONFIG_DOORLOCK_UNLOCK_MS);
        } else {
            printf("AUTH,result=deny,stage=face,fp_id=%u\n", (unsigned)fr.id);
            if (retry_lockout_on_fail(&lockout, now_ms()) == RETRY_ACT_LOCKOUT) {
                printf("AUTH,lockout=enter,cooldown_ms=%u\n", (unsigned)(lockout.unlock_at_ms - now_ms()));
                relay_control_lockout_enter();
            } else {
                relay_control_deny();
            }
        }
    }
}

} // namespace

void auth_task_start(void *recog_task)
{
    relay_control_init();
    xTaskCreatePinnedToCore(auth_task, "auth_fsm", 4096, recog_task, 4, nullptr, 1);
}

#else

void auth_task_start(void *) {}

#endif
