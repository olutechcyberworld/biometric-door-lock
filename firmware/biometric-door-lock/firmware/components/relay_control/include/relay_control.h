#pragma once
/* Drives the solenoid lock relay, green/red status LEDs, and buzzer for Phase 3.
 * GPIOs come from Kconfig (menu "Door lock") - see docs/WIRING.md for the pin budget; defaults are the "Phase 3
 * candidates" already reserved there (14, 21, 38, 39), NOT verified against your board's schematic yet. */
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void relay_control_init(void);            /* configure all outputs (low) and the button input (pull-up) */
void relay_control_unlock(uint32_t ms);   /* green LED solid + 2 short beeps, energize relay for ms. Blocks for ms. */
void relay_control_deny(void);            /* one failed attempt: 2 quick red flashes + 1 long buzz. Blocks ~700 ms. */
void relay_control_lockout_enter(void);   /* cooldown just started: buzzer sounds once. Does not block for long. */
void relay_control_lockout_set(bool on);  /* solid red LED for as long as the cooldown lasts; off once served. */
void relay_control_white_led(bool on);    /* face-check stage indicator, near the camera. */

/* Green LED blinks on its own (via an esp_timer) while a session waits for a finger, so the blink doesn't
 * depend on polling from whatever task is blocked waiting on the sensor. Call _stop() as soon as the
 * fingerprint stage resolves (match, no-match, OR timeout) - never leave it blinking into the next stage. */
void relay_control_green_blink_start(void);
void relay_control_green_blink_stop(void);

/* Wake/start button: internal pull-up, wired to GND, so pressed reads as logic low. Debouncing is the
 * caller's job (main/auth_task.cpp) - this just reports the instantaneous raw pin state. */
bool relay_control_button_pressed(void);

#ifdef __cplusplus
}
#endif
