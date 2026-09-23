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
        printf("FPINIT,result=fail,code=%d,reason=%s\n", r, as608_strerror(r));
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

static void do_identify()
{
    printf("FP,identify,step=place\n");
    as608_identify_t o;
    int r = as608_identify(&s_dev, 8000, &o);
    if (r == AS608_OK) {
        printf("FP,identify,result=match,id=%u,score=%u,wait_ms=%u,capture_ms=%u,search_ms=%u\n", (unsigned)o.id,
               (unsigned)o.score, (unsigned)o.wait_ms, (unsigned)o.capture_ms, (unsigned)o.search_ms);
    } else if (r == AS608_NOT_FOUND) {
        printf("FP,identify,result=nomatch,wait_ms=%u,capture_ms=%u,search_ms=%u\n", (unsigned)o.wait_ms,
               (unsigned)o.capture_ms, (unsigned)o.search_ms);
    } else if (r == AS608_ERR_TIMEOUT) {
        printf("FP,identify,result=timeout\n");
    } else {
        printf("FP,identify,result=error,code=%d,reason=%s\n", r, as608_strerror(r));
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
        if (c == 'p') {
            probe();
            continue;
        }
        if (!s_ready && !probe()) {
            continue; // probe() already printed why
        }
        switch (c) {
        case 'f': do_identify(); break;
        case 'n': do_enroll(); break;
        case 'X': do_empty(); break;
        default: break;
        }
    }
}

void fingerprint_start()
{
    s_queue = xQueueCreate(4, sizeof(char));
    xTaskCreatePinnedToCore(fingerprint_task, "fingerprint", 4096, nullptr, 3, nullptr, 0);
}

bool fingerprint_post(char cmd)
{
    return s_queue && xQueueSend(s_queue, &cmd, 0) == pdTRUE;
}

#else // fingerprint disabled in menuconfig

void fingerprint_start() {}
bool fingerprint_post(char) { return false; }

#endif
