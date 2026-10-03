#include "fingerprint_task.hpp"

#include "sdkconfig.h"

#if CONFIG_DOORLOCK_FINGERPRINT

#include <cstdio>

#include "as608.h"
#include "as608_uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static as608_t s_dev;
static QueueHandle_t s_queue;
static bool s_uart_ok;
static bool s_ready;
static int64_t s_empty_armed_until_us;
static volatile bool s_busy; // true while an operation owns the sensor; new commands are refused, not queued
static QueueHandle_t s_auth_req_queue;    // holds one uint32_t timeout_ms; posted by fingerprint_post_auth_identify()
static QueueHandle_t s_auth_result_queue; // holds one fingerprint_auth_result_t; consumed by fingerprint_get_auth_result()

static void print_state()
{
    uint16_t n = 0;
    if (as608_template_count(&s_dev, &n) == AS608_OK) {
        printf("FPSTATE,count=%u,capacity=%u\n", (unsigned)n, (unsigned)s_dev.capacity);
    }
}

static bool probe()
{
    if (!s_uart_ok) {
        printf("FPINIT,result=fail,code=%d,reason=UART could not be opened (check AS608 GPIO settings)\n", AS608_ERR_COMM);
        s_ready = false;
        return false;
    }
    int r = as608_verify_password(&s_dev);
    as608_params_t p = {};
    if (r == AS608_OK) {
        r = as608_read_params(&s_dev, &p);
    }
    if (r != AS608_OK) {
        printf("FPINIT,result=fail,code=%d,rx=%u,stage=%u,reason=%s\n", r, (unsigned)s_dev.dbg_rx,
               (unsigned)s_dev.dbg_stage, as608_strerror(r));
        s_ready = false;
        return false;
    }
    uint16_t n = 0;
    as608_template_count(&s_dev, &n);
    printf("FPINIT,result=ok,capacity=%u,count=%u,security=%u,baud=%u,addr=%08X\n", (unsigned)p.capacity, (unsigned)n,
           (unsigned)p.security_level, (unsigned)p.baud_multiplier * 9600u, (unsigned)p.device_address);
    s_ready = true;
    return true;
}

static int identify_once(as608_identify_t *o, uint32_t timeout_ms)
{
    printf("FP,identify,step=place\n");
    int r = as608_identify(&s_dev, timeout_ms, o);
    if (r == AS608_OK) {
        printf("FP,identify,result=match,id=%u,score=%u,wait_ms=%u,capture_ms=%u,search_ms=%u\n", (unsigned)o->id,
               (unsigned)o->score, (unsigned)o->wait_ms, (unsigned)o->capture_ms, (unsigned)o->search_ms);
    } else if (r == AS608_NOT_FOUND) {
        printf("FP,identify,result=nomatch,wait_ms=%u,capture_ms=%u,search_ms=%u\n", (unsigned)o->wait_ms,
               (unsigned)o->capture_ms, (unsigned)o->search_ms);
    } else if (r == AS608_ERR_TIMEOUT) {
        printf("FP,identify,result=timeout\n");
    } else {
        printf("FP,identify,result=error,code=%d,reason=%s\n", r, as608_strerror(r));
    }
    return r;
}

static void do_identify()
{
    as608_identify_t o;
    identify_once(&o, 8000);
}

static void do_auth_identify(uint32_t timeout_ms)
{
    as608_identify_t o = {};
    int r = identify_once(&o, timeout_ms);
    fingerprint_auth_result_t res = {};
    res.outcome = (r == AS608_OK)            ? FP_AUTH_MATCH
                  : (r == AS608_NOT_FOUND)   ? FP_AUTH_NOMATCH
                  : (r == AS608_ERR_TIMEOUT) ? FP_AUTH_TIMEOUT
                                             : FP_AUTH_ERROR;
    res.id = o.id;
    res.score = o.score;
    if (s_auth_result_queue) {
        xQueueOverwrite(s_auth_result_queue, &res);
    }
}

static void enroll_cb(as608_enroll_step_t st, void *)
{
    static const char *names[] = {"place1", "remove", "place2", "storing"};
    printf("FP,enroll,step=%s\n", names[st]);
}

static void do_enroll()
{
    uint16_t id = 0, used = 0;
    int r = as608_next_free_id(&s_dev, &id, &used);
    if (r != AS608_OK) {
        printf("FP,enroll,result=fail,code=%d,reason=%s\n", r, as608_strerror(r));
        return;
    }
    printf("FP,enroll,step=start,id=%u\n", (unsigned)id);
    int64_t t0 = esp_timer_get_time();
    r = as608_enroll(&s_dev, id, 15000, enroll_cb, nullptr);
    unsigned ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    if (r < 0 && r != AS608_ERR_TIMEOUT) {
        // Link error (lost/garbled reply). The module may still have executed the last command, e.g. the store.
        // Compare the library count instead of trusting the reply, so a retry cannot silently burn another slot.
        uint16_t after = 0;
        if (as608_template_count(&s_dev, &after) == AS608_OK && after > used) {
            printf("FP,enroll,note=reply lost but template was stored\n");
            r = AS608_OK;
        }
    }
    if (r == AS608_OK) {
        printf("FP,enroll,result=ok,id=%u,ms=%u\n", (unsigned)id, ms);
        print_state();
    } else {
        printf("FP,enroll,result=fail,code=%d,reason=%s\n", r, as608_strerror(r));
    }
}

static void do_empty()
{
    int64_t now = esp_timer_get_time();
    if (now > s_empty_armed_until_us) {
        s_empty_armed_until_us = now + 5000000;
        printf("FP,empty,result=confirm,reason=send X again within 5 s to erase every stored fingerprint\n");
        return;
    }
    s_empty_armed_until_us = 0;
    int r = as608_empty(&s_dev);
    if (r == AS608_OK) {
        printf("FP,empty,result=ok\n");
        print_state();
    } else {
        printf("FP,empty,result=fail,code=%d,reason=%s\n", r, as608_strerror(r));
    }
}

static void fingerprint_task(void *)
{
    s_uart_ok = (as608_uart_open(&s_dev) == AS608_OK);
    for (int i = 0; i < 3 && !probe(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    for (;;) {
        char c;
        if (xQueueReceive(s_queue, &c, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_busy = true;
        if (c == 'p') {
            probe();
        } else if (s_ready || probe()) { // probe() already printed why if it fails
            switch (c) {
            case 'f': do_identify(); break;
            case 'n': do_enroll(); break;
            case 'X': do_empty(); break;
            case 'A': {
                uint32_t timeout_ms = 8000;
                if (s_auth_req_queue) {
                    xQueueReceive(s_auth_req_queue, &timeout_ms, 0);
                }
                do_auth_identify(timeout_ms);
                break;
            }
            default: break;
            }
        }
        xQueueReset(s_queue); // anything that slipped in while we were busy is stale: never run it later
        s_busy = false;
    }
}

void fingerprint_start()
{
    s_queue = xQueueCreate(4, sizeof(char));
    s_auth_req_queue = xQueueCreate(1, sizeof(uint32_t));
    s_auth_result_queue = xQueueCreate(1, sizeof(fingerprint_auth_result_t));
    xTaskCreatePinnedToCore(fingerprint_task, "fingerprint", 4096, nullptr, 3, nullptr, 0);
}

bool fingerprint_post(char cmd)
{
    if (!s_queue) {
        return false;
    }
    if (s_busy) {
        printf("FP,busy,cmd=%c,reason=another fingerprint operation is still running; command ignored\n", cmd);
        return false;
    }
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        printf("FP,busy,cmd=%c,reason=queue full; command ignored\n", cmd);
        return false;
    }
    return true;
}

bool fingerprint_post_auth_identify(uint32_t timeout_ms)
{
    if (!s_queue || !s_auth_req_queue || s_busy) {
        return false; // sensor is busy with a manual command; the auth loop retries shortly, it never queues up
    }
    xQueueOverwrite(s_auth_req_queue, &timeout_ms);
    char c = 'A';
    return xQueueSend(s_queue, &c, 0) == pdTRUE;
}

bool fingerprint_get_auth_result(fingerprint_auth_result_t *out, uint32_t wait_ms)
{
    return s_auth_result_queue && xQueueReceive(s_auth_result_queue, out, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

#else // fingerprint disabled in menuconfig

void fingerprint_start() {}
bool fingerprint_post(char) { return false; }
bool fingerprint_post_auth_identify(uint32_t) { return false; }
bool fingerprint_get_auth_result(fingerprint_auth_result_t *, uint32_t) { return false; }

#endif
