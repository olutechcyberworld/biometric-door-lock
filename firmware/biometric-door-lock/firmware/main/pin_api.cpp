#include "pin_api.hpp"

#include <cstdio>
#include <cstring>
#include "cJSON.h"
#include "device_link.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "fingerprint_task.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pin_auth.h"
#include "sdkconfig.h"
#if CONFIG_DOORLOCK_AUTH_AUTO
#include "relay_control.h"
#endif

namespace {

const char *TAG = "pin_api";
constexpr const char *NVS_NS = "pin";
constexpr const char *NVS_KEY = "ver"; // SHA256(pin): the HMAC key. Never the PIN itself.
constexpr uint32_t SETUP_MIN_GAP_MS = 10000; // spacing between finger windows opened for setup/change

pin_auth_t s_pin;
SemaphoreHandle_t s_mu;
uint32_t s_last_window_end_ms;
bool s_had_window;

uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

struct Lock {
    Lock() { xSemaphoreTake(s_mu, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mu); }
};

// ---------- NVS ----------

bool nvs_load(uint8_t v[PIN_VERIFIER_LEN])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = PIN_VERIFIER_LEN;
    bool ok = nvs_get_blob(h, NVS_KEY, v, &len) == ESP_OK && len == PIN_VERIFIER_LEN;
    nvs_close(h);
    return ok;
}

bool nvs_store(const uint8_t v[PIN_VERIFIER_LEN])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(h, NVS_KEY, v, PIN_VERIFIER_LEN) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void nvs_erase()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------- HTTP helpers ----------

esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

cJSON *read_json(httpd_req_t *req)
{
    char buf[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        return nullptr;
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) {
            return nullptr;
        }
        got += r;
    }
    buf[len] = 0;
    return cJSON_Parse(buf);
}

const char *json_str(cJSON *j, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : nullptr;
}

bool query_token(httpd_req_t *req, char *tok, size_t cap)
{
    char q[96];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, "token", tok, cap) == ESP_OK;
}

// ---------- physical gate: an ENROLLED fingerprint at the door ----------
// Setting or changing the PIN needs the owner standing at the door, not just a device on the Wi-Fi.

enum class Gate { Ok, Busy, NoFinger, Mismatch, Error };

Gate require_enrolled_finger()
{
#if CONFIG_DOORLOCK_AUTH_AUTO
    relay_control_green_blink_start(); // "place your finger"
#endif
    Gate g = Gate::Error;
    if (!fingerprint_post_auth_identify(CONFIG_DOORLOCK_PIN_FINGER_WAIT_MS)) {
        g = Gate::Busy; // a door session or a console command owns the sensor right now
    } else {
        fingerprint_auth_result_t fr = {};
        if (fingerprint_get_auth_result(&fr, CONFIG_DOORLOCK_PIN_FINGER_WAIT_MS + 1500)) {
            switch (fr.outcome) {
            case FP_AUTH_MATCH: g = Gate::Ok; break;
            case FP_AUTH_NOMATCH: g = Gate::Mismatch; break;
            case FP_AUTH_TIMEOUT: g = Gate::NoFinger; break;
            default: g = Gate::Error; break;
            }
        }
    }
#if CONFIG_DOORLOCK_AUTH_AUTO
    relay_control_green_blink_stop();
    if (g == Gate::Ok) {
        relay_control_beep(120);
    } else if (g == Gate::Mismatch) {
        relay_control_deny();
    }
#endif
    return g;
}

esp_err_t gate_error(httpd_req_t *req, Gate g)
{
    switch (g) {
    case Gate::Busy: return send_json(req, "409 Conflict", "{\"error\":\"sensor_busy\"}");
    case Gate::NoFinger: return send_json(req, "408 Request Timeout", "{\"error\":\"no_finger\"}");
    case Gate::Mismatch: return send_json(req, "403 Forbidden", "{\"error\":\"fingerprint_mismatch\"}");
    default: return send_json(req, "503 Service Unavailable", "{\"error\":\"sensor_error\"}");
    }
}

// ---------- handlers ----------

esp_err_t status_get(httpd_req_t *req)
{
    bool has;
    uint32_t locked_ms = 0;
    {
        Lock l;
        has = s_pin.has_pin;
        if (has && retry_lockout_is_locked(&s_pin.lockout, now_ms())) {
            locked_ms = s_pin.lockout.unlock_at_ms - now_ms();
        }
    }
    char b[64];
    snprintf(b, sizeof(b), "{\"pin_set\":%s,\"locked_ms\":%u}", has ? "true" : "false", (unsigned)locked_ms);
    return send_json(req, "200 OK", b);
}

esp_err_t challenge_get(httpd_req_t *req)
{
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    char nonce[PIN_NONCE_HEX + 1];
    {
        Lock l;
        if (!s_pin.has_pin) {
            return send_json(req, "409 Conflict", "{\"error\":\"no_pin_set\"}");
        }
        pin_auth_new_challenge(&s_pin, now_ms(), r, nonce);
    }
    char b[64];
    snprintf(b, sizeof(b), "{\"nonce\":\"%s\"}", nonce);
    return send_json(req, "200 OK", b);
}

esp_err_t auth_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_json\"}");
    }
    const char *nonce = json_str(j, "nonce");
    const char *resp = json_str(j, "response");
    if (!nonce || !resp) {
        cJSON_Delete(j);
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_json\"}");
    }
    uint8_t t[16];
    esp_fill_random(t, sizeof(t));
    char tok[PIN_TOKEN_HEX + 1] = {0};
    int left = 0;
    uint32_t ms = 0;
    pin_verify_result_t r;
    {
        Lock l;
        r = pin_auth_verify(&s_pin, now_ms(), nonce, resp, t, tok, &left, &ms);
    }
    cJSON_Delete(j);

    char b[128];
    switch (r) {
    case PIN_OK:
        printf("PIN,auth=ok\n");
        snprintf(b, sizeof(b), "{\"session_token\":\"%s\",\"expires_in\":%u}", tok, (unsigned)PIN_SESSION_TTL_S);
        return send_json(req, "200 OK", b);
    case PIN_ERR_NO_PIN:
        return send_json(req, "409 Conflict", "{\"error\":\"no_pin_set\"}");
    case PIN_ERR_LOCKED:
        printf("PIN,auth=locked,cooldown_ms=%u\n", (unsigned)ms);
        snprintf(b, sizeof(b), "{\"error\":\"locked_out\",\"attempts_remaining\":0,\"retry_after_ms\":%u}", (unsigned)ms);
        return send_json(req, "401 Unauthorized", b);
    case PIN_ERR_BAD_NONCE:
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_nonce\"}");
    default:
        printf("PIN,auth=fail,attempts_remaining=%d\n", left);
        snprintf(b, sizeof(b), "{\"error\":\"invalid_pin\",\"attempts_remaining\":%d}", left);
        return send_json(req, "401 Unauthorized", b);
    }
}

bool parse_hash_field(httpd_req_t *req, const char *field, uint8_t out[PIN_VERIFIER_LEN])
{
    cJSON *j = read_json(req);
    if (!j) {
        return false;
    }
    const char *s = json_str(j, field);
    bool ok = s && pin_auth_parse_verifier_hex(s, out);
    cJSON_Delete(j);
    return ok;
}

// First-time PIN: only while no PIN exists, and only with an enrolled finger on the sensor.
esp_err_t setup_post(httpd_req_t *req)
{
    uint8_t ver[PIN_VERIFIER_LEN];
    if (!parse_hash_field(req, "pin_hash", ver)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_pin_hash\"}");
    }
    {
        Lock l;
        if (s_pin.has_pin) {
            return send_json(req, "409 Conflict", "{\"error\":\"pin_already_set\"}");
        }
        if (s_had_window && now_ms() - s_last_window_end_ms < SETUP_MIN_GAP_MS) {
            return send_json(req, "429 Too Many Requests", "{\"error\":\"too_soon\"}");
        }
    }
    Gate g = require_enrolled_finger();
    {
        Lock l;
        s_last_window_end_ms = now_ms();
        s_had_window = true;
    }
    if (g != Gate::Ok) {
        return gate_error(req, g);
    }
    Lock l;
    if (s_pin.has_pin) {
        return send_json(req, "409 Conflict", "{\"error\":\"pin_already_set\"}");
    }
    if (!nvs_store(ver)) {
        return send_json(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
    }
    pin_auth_set_verifier(&s_pin, ver);
    printf("PIN,set=ok\n");
    return send_json(req, "200 OK", "{\"status\":\"pin_set\"}");
}

// Change PIN: a live session (old PIN proven via challenge-response) AND an enrolled finger. The session ends.
esp_err_t change_post(httpd_req_t *req)
{
    char tok[PIN_TOKEN_HEX + 4];
    bool have_tok = query_token(req, tok, sizeof(tok));
    {
        Lock l;
        if (!have_tok || !pin_auth_session_valid(&s_pin, now_ms(), tok)) {
            return send_json(req, "401 Unauthorized", "{\"error\":\"session_expired\"}");
        }
    }
    uint8_t ver[PIN_VERIFIER_LEN];
    if (!parse_hash_field(req, "new_pin_hash", ver)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_pin_hash\"}");
    }
    Gate g = require_enrolled_finger();
    {
        Lock l;
        s_last_window_end_ms = now_ms();
        s_had_window = true;
    }
    if (g != Gate::Ok) {
        return gate_error(req, g);
    }
    Lock l;
    if (!pin_auth_session_valid(&s_pin, now_ms(), tok)) { // the session may have run out while waiting for the finger
        return send_json(req, "401 Unauthorized", "{\"error\":\"session_expired\"}");
    }
    if (!nvs_store(ver)) {
        return send_json(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
    }
    pin_auth_set_verifier(&s_pin, ver); // also ends the session: the app logs in again with the new PIN
    printf("PIN,change=ok\n");
    return send_json(req, "200 OK", "{\"status\":\"pin_changed\"}");
}

void register_handlers(httpd_handle_t srv)
{
    static const httpd_uri_t u_status = {.uri = "/pin/status", .method = HTTP_GET, .handler = status_get, .user_ctx = nullptr};
    static const httpd_uri_t u_chal = {.uri = "/challenge", .method = HTTP_GET, .handler = challenge_get, .user_ctx = nullptr};
    static const httpd_uri_t u_auth = {.uri = "/auth", .method = HTTP_POST, .handler = auth_post, .user_ctx = nullptr};
    static const httpd_uri_t u_setup = {.uri = "/pin/setup", .method = HTTP_POST, .handler = setup_post, .user_ctx = nullptr};
    static const httpd_uri_t u_change = {.uri = "/pin/change", .method = HTTP_POST, .handler = change_post, .user_ctx = nullptr};
    httpd_register_uri_handler(srv, &u_status);
    httpd_register_uri_handler(srv, &u_chal);
    httpd_register_uri_handler(srv, &u_auth);
    httpd_register_uri_handler(srv, &u_setup);
    httpd_register_uri_handler(srv, &u_change);
}

} // namespace

void pin_api_start()
{
    retry_lockout_cfg_t lc = {};
    lc.max_attempts = CONFIG_DOORLOCK_PIN_MAX_ATTEMPTS;
    lc.cooldown_ms[0] = 30000; // same escalation shape as the door lockout; kept separate on purpose
    lc.cooldown_ms[1] = 60000;
    lc.cooldown_ms[2] = 300000;
    s_mu = xSemaphoreCreateMutex();
    pin_auth_init(&s_pin, lc);
    uint8_t v[PIN_VERIFIER_LEN];
    if (nvs_load(v)) {
        pin_auth_set_verifier(&s_pin, v);
    }
    printf("PIN,state=%s\n", s_pin.has_pin ? "set" : "unset");
    device_link_add_registrar(register_handlers);
}

bool pin_api_session_valid(const char *token)
{
    Lock l;
    return pin_auth_session_valid(&s_pin, now_ms(), token);
}

void pin_api_session_end()
{
    Lock l;
    pin_auth_session_end(&s_pin);
}

bool pin_api_console_reset()
{
    static int64_t armed_us = 0;
    int64_t now = esp_timer_get_time();
    if (armed_us && now - armed_us < 5000000) {
        armed_us = 0;
        Lock l;
        nvs_erase();
        pin_auth_clear(&s_pin);
        printf("PIN,reset=done\n");
        return true;
    }
    armed_us = now;
    printf("PIN,reset=armed,send P again within 5 s to erase the PIN\n");
    return false;
}
