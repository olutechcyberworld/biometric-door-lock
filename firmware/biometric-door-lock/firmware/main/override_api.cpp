#include "override_api.hpp"

#include <cstdio>
#include <cstring>
#include "device_link.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "fingerprint_task.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_api.hpp"
#include "sdkconfig.h"
#include "user_db.hpp"
#if CONFIG_DOORLOCK_AUTH_AUTO
#include "relay_control.h"
#endif

#if CONFIG_DOORLOCK_AUTH_AUTO

namespace {

constexpr uint32_t WINDOW_MS = 90000;
constexpr int MAX_WRONG_FINGERS = 3; // a wrong finger does not end the window, but three of them do
volatile bool s_active;

uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// The first identify was posted by the HTTP handler (so "sensor busy" could be answered synchronously); this task
// collects it and re-posts for the time that is left after a finger that is not enrolled.
void waiter_task(void *)
{
    const uint32_t t0 = now_ms();
    int wrong = 0;
    bool granted = false;
    for (;;) {
        uint32_t used = now_ms() - t0;
        if (used >= WINDOW_MS) break;
        fingerprint_auth_result_t fr = {};
        bool got = fingerprint_get_auth_result(&fr, WINDOW_MS - used + 1500);
        if (!got) break;
        if (fr.outcome == FP_AUTH_MATCH) {
            // once users exist a finger that belongs to nobody is not allowed to open anything
            if (user_db_count() > 0 && !user_db_has_fp_slot(fr.id)) {
                printf("OVERRIDE,result=deny,reason=finger_not_bound,fp_id=%u\n", (unsigned)fr.id);
                relay_control_deny();
                wrong++;
            } else {
                printf("OVERRIDE,result=grant,fp_id=%u\n", (unsigned)fr.id);
                relay_control_green_blink_stop();
                relay_control_unlock(CONFIG_DOORLOCK_UNLOCK_MS);
                granted = true;
                break;
            }
        } else if (fr.outcome == FP_AUTH_NOMATCH) {
            printf("OVERRIDE,result=deny,reason=nomatch\n");
            relay_control_deny();
            wrong++;
        } else if (fr.outcome == FP_AUTH_TIMEOUT) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(500)); // sensor error: back off, keep the window
        }
        if (wrong >= MAX_WRONG_FINGERS) {
            printf("OVERRIDE,result=end,reason=too_many_wrong_fingers\n");
            break;
        }
        // ask the sensor again for what is left of the window (it clears its busy flag just after posting a result)
        bool posted = false;
        for (int i = 0; i < 20 && !posted; i++) {
            uint32_t left = (now_ms() - t0 < WINDOW_MS) ? WINDOW_MS - (now_ms() - t0) : 0;
            if (left < 1000) break;
            posted = fingerprint_post_auth_identify(left);
            if (!posted) vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!posted) break;
        relay_control_green_blink_start();
    }
    relay_control_green_blink_stop();
    if (!granted) printf("OVERRIDE,result=end,reason=window_closed\n");
    s_active = false;
    vTaskDelete(nullptr);
}

esp_err_t override_post(httpd_req_t *req)
{
    char q[96], tok[40];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "token", tok, sizeof(tok)) != ESP_OK ||
        !pin_api_session_valid(tok)) {
        return send_json(req, "401 Unauthorized", "{\"error\":\"session_expired\"}");
    }
    if (s_active) {
        return send_json(req, "409 Conflict", "{\"error\":\"override_active\"}");
    }
    relay_control_green_blink_start();
    if (!fingerprint_post_auth_identify(WINDOW_MS)) {
        relay_control_green_blink_stop();
        return send_json(req, "409 Conflict", "{\"error\":\"sensor_busy\"}");
    }
    s_active = true;
    if (xTaskCreate(waiter_task, "override", 4096, nullptr, 4, nullptr) != pdPASS) {
        s_active = false;
        relay_control_green_blink_stop();
        return send_json(req, "500 Internal Server Error", "{\"error\":\"no_task\"}");
    }
    pin_api_session_end(); // the token has done its job
    printf("OVERRIDE,window=open,timeout_ms=%u\n", (unsigned)WINDOW_MS);
    char b[96];
    snprintf(b, sizeof(b), "{\"status\":\"awaiting_fingerprint\",\"timeout_ms\":%u}", (unsigned)WINDOW_MS);
    return send_json(req, "200 OK", b);
}

void register_handlers(httpd_handle_t srv)
{
    static httpd_uri_t u = {};
    u.uri = "/override";
    u.method = HTTP_POST;
    u.handler = override_post;
    httpd_register_uri_handler(srv, &u);
}

} // namespace

bool override_api_active()
{
    return s_active;
}

void override_api_start()
{
    device_link_add_registrar(register_handlers);
}

#else

bool override_api_active() { return false; }
void override_api_start() {}

#endif
