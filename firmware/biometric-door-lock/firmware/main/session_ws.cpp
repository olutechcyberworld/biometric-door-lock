#include "session_ws.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "sdkconfig.h"

#if CONFIG_DOORLOCK_SESSION_WS && CONFIG_DOORLOCK_AUTH_AUTO

#include "cJSON.h"
#include "device_link.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "face_auth_bridge.hpp"
#include "fingerprint_task.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_lite.h"
#include "pin_api.hpp"
#include "relay_control.h"
#include "user_db.hpp"
#include "who_bench.hpp"

using who::recognition::WhoRecognitionCore;

namespace {

const char *TAG = "session_ws";
constexpr uint32_t GATE_WAIT_MS = 20000;      // owner's finger after connecting
constexpr uint32_t IDLE_TIMEOUT_MS = 120000;  // no message from the app
constexpr uint32_t FP_STEP_TIMEOUT_MS = 15000;
constexpr int FRAMES_NEEDED = 5;
constexpr int64_t STREAM_INTERVAL_US = 220000; // ~4.5 preview frames per second
constexpr size_t JPG_CAP = 32768;

struct Msg {
    enum Kind : uint8_t { SelectEnroll, SelectManage, Capture, Delete, Done, Closed } kind;
    char name[32];
    uint16_t id;
};

struct Session {
    volatile bool active;
    volatile bool stop;
    volatile bool streaming;
    httpd_handle_t hd;
    int fd;
    QueueHandle_t q;
} S;

WhoRecognitionCore *s_recog;
SemaphoreHandle_t s_send_mu;
TaskHandle_t s_stream_task;

// frame copied by the detector tap, encoded + sent by stream_task (so a slow phone can never stall the detector)
uint8_t *s_frame;   // RGB565 copy
uint8_t *s_jpg;     // encoded preview
size_t s_frame_cap;
volatile bool s_frame_busy;
uint16_t s_fw, s_fh;
dl::image::pix_type_t s_fpix;
bool s_has_box;
int s_box[4];
int64_t s_last_tap_us;

bool ws_send(httpd_ws_type_t type, const uint8_t *data, size_t len)
{
    if (!S.active || !s_send_mu) return false;
    if (device_link_server() != S.hd) { // the Wi-Fi link dropped and the server was restarted: our socket is gone
        S.stop = true;
        return false;
    }
    xSemaphoreTake(s_send_mu, portMAX_DELAY);
    httpd_ws_frame_t f = {};
    f.type = type;
    f.payload = const_cast<uint8_t *>(data);
    f.len = len;
    f.final = true;
    esp_err_t e = httpd_ws_send_frame_async(S.hd, S.fd, &f);
    xSemaphoreGive(s_send_mu);
    if (e != ESP_OK) {
        printf("SESSION,send=failed,err=%d\n", (int)e);
        S.stop = true; // the phone went away
        return false;
    }
    return true;
}

bool send_json(const char *s)
{
    return ws_send(HTTPD_WS_TYPE_TEXT, reinterpret_cast<const uint8_t *>(s), strlen(s));
}

void send_state(const char *st)
{
    char b[80];
    snprintf(b, sizeof(b), "{\"type\":\"state\",\"state\":\"%s\"}", st);
    send_json(b);
}

// ---------- live preview ----------

void stream_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (S.streaming && s_frame_busy) {
            jpeglite_fmt_t fmt = s_fpix == dl::image::DL_IMAGE_PIX_TYPE_RGB565LE ? JPEGLITE_RGB565_LE : JPEGLITE_RGB565_BE;
            int n = jpeglite_encode(s_frame, s_fw, s_fh, fmt, CONFIG_DOORLOCK_STREAM_JPEG_QUALITY, s_jpg, JPG_CAP);
            if (n > 0 && ws_send(HTTPD_WS_TYPE_BINARY, s_jpg, (size_t)n)) {
                char b[200];
                if (s_has_box) {
                    snprintf(b, sizeof(b),
                             "{\"type\":\"detection\",\"face_found\":true,\"bbox\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
                             "\"frame_w\":%u,\"frame_h\":%u}",
                             s_box[0], s_box[1], s_box[2] - s_box[0], s_box[3] - s_box[1], (unsigned)s_fw, (unsigned)s_fh);
                } else {
                    snprintf(b, sizeof(b), "{\"type\":\"detection\",\"face_found\":false,\"frame_w\":%u,\"frame_h\":%u}",
                             (unsigned)s_fw, (unsigned)s_fh);
                }
                send_json(b);
            }
        }
        s_frame_busy = false;
    }
}

// Runs in the detector task for every frame: must stay cheap (a memcpy), never blocks on the network.
void frame_tap(const dl::image::img_t &img, const std::list<dl::detect::result_t> &res)
{
    if (!S.streaming || s_frame_busy || !s_frame || !s_stream_task) return;
    if (img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565BE && img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565LE) return;
    size_t bytes = (size_t)img.width * img.height * 2;
    if (bytes > s_frame_cap) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_tap_us < STREAM_INTERVAL_US) return;
    s_last_tap_us = now;
    memcpy(s_frame, img.data, bytes);
    s_fw = img.width;
    s_fh = img.height;
    s_fpix = img.pix_type;
    s_has_box = false;
    int best = 0;
    for (const auto &r : res) {
        if (r.box.size() >= 4 && r.box_area() > best) {
            best = r.box_area();
            for (int i = 0; i < 4; i++) s_box[i] = r.box[i];
            s_has_box = true;
        }
    }
    s_frame_busy = true;
    xTaskNotifyGive(s_stream_task);
}

// ---------- session state machine ----------

bool wait_msg(Msg *m, uint32_t ms)
{
    return xQueueReceive(S.q, m, pdMS_TO_TICKS(ms)) == pdTRUE && !S.stop;
}

void rollback_faces(const uint16_t *ids, int n)
{
    for (int i = 0; i < n; i++) face_delete(ids[i]);
}

void fp_progress(int step, void *)
{
    static const char *names[] = {"place1", "remove", "place2", "storing"};
    char b[80];
    snprintf(b, sizeof(b), "{\"type\":\"fingerprint_progress\",\"step\":\"%s\"}", names[step & 3]);
    send_json(b);
}

void fail_enroll(const char *reason)
{
    char b[96];
    snprintf(b, sizeof(b), "{\"type\":\"enrollment_failed\",\"reason\":\"%s\"}", reason);
    send_json(b);
}

void do_enroll(const char *raw_name)
{
    char name[USER_NAME_MAX];
    if (!user_store_clean_name(raw_name, name)) {
        fail_enroll("bad_name");
        return;
    }
    if (user_db_count() >= USER_MAX) {
        fail_enroll("user_table_full");
        return;
    }
    uint16_t faces[FRAMES_NEEDED];
    int n = 0;
    S.streaming = true;
    while (n < FRAMES_NEEDED) {
        Msg m;
        if (!wait_msg(&m, IDLE_TIMEOUT_MS) || m.kind == Msg::Done || m.kind == Msg::Closed) {
            S.streaming = false;
            rollback_faces(faces, n);
            return; // nothing half-enrolled is kept
        }
        if (m.kind != Msg::Capture) continue;
        uint16_t id = 0;
        bool ok = face_enroll_blocking(s_recog, 3000, &id);
        char b[160];
        if (ok) {
            faces[n++] = id;
            snprintf(b, sizeof(b), "{\"type\":\"capture_result\",\"success\":true,\"frame_index\":%d,\"frames_needed\":%d}", n,
                     FRAMES_NEEDED);
        } else {
            snprintf(b, sizeof(b),
                     "{\"type\":\"capture_result\",\"success\":false,\"frame_index\":%d,\"frames_needed\":%d,"
                     "\"reason\":\"no_face_detected\"}",
                     n, FRAMES_NEEDED);
        }
        send_json(b);
    }
    S.streaming = false;

    // The new person's finger: two scans of the same finger into the next free slot.
    send_state("awaiting_new_fingerprint");
    if (!fingerprint_post_enroll(FP_STEP_TIMEOUT_MS, fp_progress, nullptr)) {
        rollback_faces(faces, n);
        fail_enroll("sensor_busy");
        return;
    }
    fingerprint_enroll_result_t fr = {};
    bool got = fingerprint_get_enroll_result(&fr, 4 * FP_STEP_TIMEOUT_MS + 5000);
    if (!got || fr.outcome != FP_ENROLL_OK) {
        rollback_faces(faces, n);
        fail_enroll(!got                              ? "sensor_error"
                    : fr.outcome == FP_ENROLL_TIMEOUT ? "fingerprint_timeout"
                    : fr.outcome == FP_ENROLL_NOMATCH ? "fingerprint_mismatch"
                    : fr.outcome == FP_ENROLL_FULL    ? "fingerprint_library_full"
                                                      : "sensor_error");
        return;
    }
    uint16_t uid = user_db_add(name, fr.id, faces, FRAMES_NEEDED);
    if (!uid) {
        rollback_faces(faces, n);
        fingerprint_delete_slot(fr.id, 3000);
        fail_enroll("storage");
        return;
    }
    printf("USERS,added,id=%u,fp=%u\n", (unsigned)uid, (unsigned)fr.id);
    char b[96];
    snprintf(b, sizeof(b), "{\"type\":\"enrollment_complete\",\"user_id\":\"%u\"}", (unsigned)uid);
    send_json(b);
}

// Deleting a person removes the finger first (it is what can open the door on its own in legacy mode), then every
// face, then the record. If the finger cannot be deleted the record stays so the owner can retry.
bool delete_user(uint16_t id)
{
    user_rec_t rec;
    if (!user_db_get(id, &rec)) return false;
    if (!fingerprint_delete_slot(rec.fp_slot, 3000)) {
        ESP_LOGW(TAG, "fingerprint slot %u could not be deleted", (unsigned)rec.fp_slot);
        return false;
    }
    for (int i = 0; i < rec.n_faces; i++) {
        if (!face_delete(rec.face_ids[i])) ESP_LOGW(TAG, "face id %u not deleted", (unsigned)rec.face_ids[i]);
    }
    bool ok = user_db_remove(id, nullptr);
    if (ok) printf("USERS,removed,id=%u\n", (unsigned)id);
    return ok;
}

void do_manage()
{
    char list[USER_MAX * 56 + 64];
    if (user_db_list_json(list, sizeof(list))) send_json(list);
    for (;;) {
        Msg m;
        if (!wait_msg(&m, IDLE_TIMEOUT_MS) || m.kind == Msg::Done || m.kind == Msg::Closed) return;
        if (m.kind != Msg::Delete) continue;
        bool ok = delete_user(m.id);
        char b[96];
        snprintf(b, sizeof(b), "{\"type\":\"delete_result\",\"success\":%s,\"id\":\"%u\"}", ok ? "true" : "false", (unsigned)m.id);
        send_json(b);
    }
}

void session_task(void *)
{
    bool ok = false;
    send_state("awaiting_fingerprint");
    relay_control_green_blink_start();
    if (fingerprint_post_auth_identify(GATE_WAIT_MS)) {
        printf("SESSION,gate=waiting_for_finger,timeout_ms=%u\n", (unsigned)GATE_WAIT_MS);
        fingerprint_auth_result_t fr = {};
        bool got = fingerprint_get_auth_result(&fr, GATE_WAIT_MS + 1500);
        relay_control_green_blink_stop();
        if (got && fr.outcome == FP_AUTH_MATCH) {
            // once users exist, only a finger that belongs to one of them may open this session
            ok = user_db_count() == 0 || user_db_has_fp_slot(fr.id);
            if (!ok) send_state("fingerprint_failed");
        } else if (got && fr.outcome == FP_AUTH_TIMEOUT) {
            send_state("timeout");
        } else {
            send_state("fingerprint_failed");
        }
    } else {
        printf("SESSION,gate=refused,reason=sensor_busy\n");
        relay_control_green_blink_stop();
        send_state("fingerprint_failed"); // sensor busy
    }
    printf("SESSION,gate=%s\n", ok ? "confirmed" : "failed");
    if (ok && !S.stop) {
        send_state("fingerprint_confirmed");
        relay_control_beep(100);
        Msg m;
        while (wait_msg(&m, IDLE_TIMEOUT_MS)) {
            if (m.kind == Msg::SelectEnroll) {
                do_enroll(m.name);
                break;
            }
            if (m.kind == Msg::SelectManage) {
                do_manage();
                break;
            }
            if (m.kind == Msg::Done || m.kind == Msg::Closed) break;
        }
    }
    S.streaming = false;
    S.stop = true;
    printf("SESSION,end\n");
    vTaskDelay(pdMS_TO_TICKS(150)); // let the last frame leave the socket
    httpd_handle_t hd = S.hd;
    int fd = S.fd;
    S.active = false;
    if (device_link_server() == hd) {
        httpd_sess_trigger_close(hd, fd);
    }
    vQueueDelete(S.q);
    S.q = nullptr;
    vTaskDelete(nullptr);
}

// ---------- HTTP / WebSocket handler ----------

void post(Msg::Kind k, const char *name = nullptr, uint16_t id = 0)
{
    if (!S.q) return;
    Msg m = {};
    m.kind = k;
    if (name) strlcpy(m.name, name, sizeof(m.name));
    m.id = id;
    xQueueSend(S.q, &m, 0);
}

esp_err_t reject(httpd_req_t *req, const char *state)
{
    char b[80];
    snprintf(b, sizeof(b), "{\"type\":\"state\",\"state\":\"%s\"}", state);
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_TEXT;
    f.payload = reinterpret_cast<uint8_t *>(b);
    f.len = strlen(b);
    f.final = true;
    httpd_ws_send_frame(req, &f);
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return ESP_OK;
}

// Runs ONCE per accepted upgrade, right after the server has sent the 101. IDF 5.5 never calls the URI handler for the
// handshake request itself (that is what this callback is for), so token check, "busy" check and session start live here.
esp_err_t on_handshake(httpd_req_t *req)
{
    const int sockfd = httpd_req_to_sockfd(req);
    printf("SESSION,handshake=rx,fd=%d\n", sockfd);
    char q[96], tok[40];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "token", tok, sizeof(tok)) != ESP_OK) {
        printf("SESSION,handshake=rejected,reason=no_token\n");
        return reject(req, "unauthorized");
    }
    if (!pin_api_session_valid(tok)) {
        printf("SESSION,handshake=rejected,reason=token_invalid_or_expired\n");
        return reject(req, "unauthorized");
    }
    if (S.active) {
        printf("SESSION,handshake=rejected,reason=session_already_active\n");
        return reject(req, "busy");
    }
    S.q = xQueueCreate(8, sizeof(Msg));
    if (!S.q) return reject(req, "busy");
    S.stop = false;
    S.streaming = false;
    S.hd = req->handle;
    S.fd = sockfd;
    S.active = true;
    pin_api_session_end(); // single use
    printf("SESSION,handshake=accepted,fd=%d\n", S.fd);
    if (xTaskCreate(session_task, "session", 6144, nullptr, 4, nullptr) != pdPASS) {
        S.active = false;
        vQueueDelete(S.q);
        S.q = nullptr;
        return reject(req, "busy");
    }
    return ESP_OK;
}

// WebSocket frames from the app (after the handshake). Control frames (PING/CLOSE) are answered by the server itself.
esp_err_t session_handler(httpd_req_t *req)
{
    if (!S.active || httpd_req_to_sockfd(req) != S.fd) {
        return ESP_FAIL; // a frame on a socket that is not our session: drop it (the server closes the socket)
    }
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_TEXT;
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) {
        post(Msg::Closed);
        return ESP_FAIL;
    }
    if (f.len == 0 || f.len > 200) {
        if (f.len > 0 && f.len <= 4096) { // drain what we will not read
            uint8_t *junk = static_cast<uint8_t *>(malloc(f.len));
            if (junk) {
                f.payload = junk;
                httpd_ws_recv_frame(req, &f, f.len);
                free(junk);
            }
        }
        return ESP_OK;
    }
    char buf[208];
    f.payload = reinterpret_cast<uint8_t *>(buf);
    if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) {
        post(Msg::Closed);
        return ESP_FAIL;
    }
    buf[f.len] = 0;
    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        post(Msg::Closed);
        return ESP_OK;
    }
    if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
    cJSON *j = cJSON_Parse(buf);
    if (!j) return ESP_OK;
    cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "type");
    const char *type = cJSON_IsString(t) ? t->valuestring : "";
    printf("SESSION,rx=%s\n", type);
    if (!strcmp(type, "select_mode")) {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "mode");
        cJSON *n = cJSON_GetObjectItemCaseSensitive(j, "name");
        const char *mode = cJSON_IsString(m) ? m->valuestring : "";
        if (!strcmp(mode, "enroll")) post(Msg::SelectEnroll, cJSON_IsString(n) ? n->valuestring : "");
        else if (!strcmp(mode, "manage")) post(Msg::SelectManage);
    } else if (!strcmp(type, "capture")) {
        post(Msg::Capture);
    } else if (!strcmp(type, "delete_user")) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
        post(Msg::Delete, nullptr, (uint16_t)(cJSON_IsString(id) ? atoi(id->valuestring) : 0));
    } else if (!strcmp(type, "done")) {
        post(Msg::Done);
    }
    cJSON_Delete(j);
    return ESP_OK;
}

void on_socket_closed(int fd)
{
    if (S.active && fd == S.fd) {
        S.stop = true;
        post(Msg::Closed); // wakes the session task if it is waiting for a message
    }
}

void register_handlers(httpd_handle_t srv)
{
    static httpd_uri_t u = {};
    u.uri = "/session";
    u.method = HTTP_GET;
    u.handler = session_handler;
    u.is_websocket = true;
#if CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
    u.ws_post_handshake_cb = on_handshake; // IDF does not call session_handler for the upgrade itself
#else
#error "DOORLOCK_SESSION_WS needs HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT (the Kconfig select should have enabled it)"
#endif
    u.handle_ws_control_frames = false; // the server answers PING and CLOSE itself; the close hook below tells us
    esp_err_t e = httpd_register_uri_handler(srv, &u);
    printf("SESSION,register=%s,err=%d\n", e == ESP_OK ? "ok" : "FAILED", (int)e);
}

} // namespace

bool session_ws_active()
{
    return S.active;
}

void session_ws_start(WhoRecognitionCore *recog)
{
    s_recog = recog;
    s_send_mu = xSemaphoreCreateMutex();
    s_frame_cap = 240 * 240 * 2;
    s_frame = static_cast<uint8_t *>(heap_caps_malloc(s_frame_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_jpg = static_cast<uint8_t *>(heap_caps_malloc(JPG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_frame || !s_jpg) {
        printf("SESSION,disabled,reason=no_psram_for_preview_buffers\n");
        return;
    }
    printf("SESSION,ready\n");
    xTaskCreatePinnedToCore(stream_task, "preview", 6144, nullptr, 2, &s_stream_task, 0);
    who::bench::g_frame_tap = &frame_tap;
    device_link_set_close_fn(on_socket_closed);
    device_link_add_registrar(register_handlers);
}

#else // session support compiled out

bool session_ws_active() { return false; }
void session_ws_start(who::recognition::WhoRecognitionCore *) {}

#endif