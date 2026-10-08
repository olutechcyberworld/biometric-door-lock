#include "relay_control.h"

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const int PINS[] = {
    CONFIG_DOORLOCK_RELAY_GPIO,
    CONFIG_DOORLOCK_LED_GREEN_GPIO,
    CONFIG_DOORLOCK_LED_RED_GPIO,
    CONFIG_DOORLOCK_BUZZER_GPIO,
    CONFIG_DOORLOCK_LED_WHITE_GPIO,
};

static inline void set(int gpio, int level)
{
    gpio_set_level((gpio_num_t)gpio, level);
}

void relay_control_init(void)
{
    uint64_t mask = 0;
    for (unsigned i = 0; i < sizeof(PINS) / sizeof(PINS[0]); i++) {
        mask |= (1ULL << PINS[i]);
    }
    gpio_config_t out_cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    for (unsigned i = 0; i < sizeof(PINS) / sizeof(PINS[0]); i++) {
        set(PINS[i], 0);
    }

    gpio_config_t button_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_DOORLOCK_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&button_cfg);
}

bool relay_control_button_pressed(void)
{
    return gpio_get_level((gpio_num_t)CONFIG_DOORLOCK_BUTTON_GPIO) == 0; // active-low, internal pull-up
}

/* Red LED arbitration: lockout (solid) and deny (flashes) win over the status blink. */
static volatile bool s_red_lockout;
static volatile bool s_red_busy;
static esp_timer_handle_t s_red_timer;
static relay_red_status_t s_red_status = RED_STATUS_OFF;
static bool s_red_status_on;

static void red_status_cb(void *arg)
{
    (void)arg;
    s_red_status_on = !s_red_status_on;
    if (s_red_lockout || s_red_busy) {
        return;
    }
    set(CONFIG_DOORLOCK_LED_RED_GPIO, s_red_status_on ? 1 : 0);
}

void relay_control_red_status(relay_red_status_t st)
{
    if (st == s_red_status) {
        return;
    }
    s_red_status = st;
    if (!s_red_timer) {
        const esp_timer_create_args_t args = {.callback = &red_status_cb, .name = "red_status"};
        esp_timer_create(&args, &s_red_timer);
    }
    if (esp_timer_is_active(s_red_timer)) {
        esp_timer_stop(s_red_timer);
    }
    s_red_status_on = false;
    if (!s_red_lockout && !s_red_busy) {
        set(CONFIG_DOORLOCK_LED_RED_GPIO, 0);
    }
    if (st == RED_STATUS_OFFLINE) {
        esp_timer_start_periodic(s_red_timer, 500 * 1000);      /* 1 Hz */
    } else if (st == RED_STATUS_PROVISIONING) {
        esp_timer_start_periodic(s_red_timer, 150 * 1000);      /* ~3 Hz */
    }
}

void relay_control_beep(uint32_t ms)
{
    set(CONFIG_DOORLOCK_BUZZER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    set(CONFIG_DOORLOCK_BUZZER_GPIO, 0);
}

static esp_timer_handle_t s_green_blink_timer;
static bool s_green_blink_on;

static void green_blink_cb(void *arg)
{
    (void)arg;
    s_green_blink_on = !s_green_blink_on;
    set(CONFIG_DOORLOCK_LED_GREEN_GPIO, s_green_blink_on ? 1 : 0);
}

void relay_control_green_blink_start(void)
{
    if (!s_green_blink_timer) {
        const esp_timer_create_args_t args = {
            .callback = &green_blink_cb,
            .name = "green_blink",
        };
        esp_timer_create(&args, &s_green_blink_timer);
    }
    s_green_blink_on = false;
    set(CONFIG_DOORLOCK_LED_GREEN_GPIO, 0);
    esp_timer_start_periodic(s_green_blink_timer, (uint64_t)CONFIG_DOORLOCK_GREEN_BLINK_MS * 1000);
}

void relay_control_green_blink_stop(void)
{
    if (s_green_blink_timer) {
        esp_timer_stop(s_green_blink_timer); // no-op (logged, not fatal) if it wasn't running
    }
    set(CONFIG_DOORLOCK_LED_GREEN_GPIO, 0);
}

void relay_control_unlock(uint32_t ms)
{
    set(CONFIG_DOORLOCK_LED_GREEN_GPIO, 1);
    for (int i = 0; i < 2; i++) { // "2 short" per the agreed UX table - distinct from deny's single long buzz
        set(CONFIG_DOORLOCK_BUZZER_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        set(CONFIG_DOORLOCK_BUZZER_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    set(CONFIG_DOORLOCK_RELAY_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(ms > 200 ? ms - 200 : ms)); // account for the beeps above, keep total unlock ~= ms
    set(CONFIG_DOORLOCK_RELAY_GPIO, 0);
    set(CONFIG_DOORLOCK_LED_GREEN_GPIO, 0);
}

void relay_control_deny(void)
{
    s_red_busy = true;
    // One failed attempt (retries still left): 2 quick red flashes, and ONE longer buzz - not a buzz per
    // flash, so this doesn't sound like the lockout tone below and train the owner to ignore both.
    for (int i = 0; i < 2; i++) {
        set(CONFIG_DOORLOCK_LED_RED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        set(CONFIG_DOORLOCK_LED_RED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    set(CONFIG_DOORLOCK_BUZZER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(250));
    set(CONFIG_DOORLOCK_BUZZER_GPIO, 0);
    s_red_busy = false;
}

void relay_control_lockout_enter(void)
{
    // Cooldown just started: one longer buzz, once, here - never repeated for the rest of the cooldown (see
    // relay_control_lockout_set below). A buzzer that keeps sounding through the whole cooldown trains people
    // to ignore it and annoys neighbours for no security benefit.
    set(CONFIG_DOORLOCK_BUZZER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(600));
    set(CONFIG_DOORLOCK_BUZZER_GPIO, 0);
}

void relay_control_lockout_set(bool on)
{
    s_red_lockout = on;
    set(CONFIG_DOORLOCK_LED_RED_GPIO, on ? 1 : 0);
}

void relay_control_white_led(bool on)
{
    set(CONFIG_DOORLOCK_LED_WHITE_GPIO, on ? 1 : 0);
}
