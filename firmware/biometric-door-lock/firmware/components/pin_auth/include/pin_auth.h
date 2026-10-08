#pragma once
/*
 * PIN challenge-response core for the owner-app link (docs/APP_PROTOCOL.md). Pure logic, no ESP-IDF - same split as
 * auth_fsm / retry_lockout: the caller supplies the clock and the random bytes and does the storage and HTTP.
 *
 * What the device keeps is the "verifier" = SHA256(pin_utf8). The app proves it knows the PIN by returning
 *     HMAC-SHA256(key = verifier, message = the nonce's hex TEXT)      (lowercase hex)
 * exactly as lib/services/crypto_service.dart computes it. The PIN itself never reaches the device.
 * Counters here (PIN attempts, cooldowns) are independent of the door's fingerprint+face lockout.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "retry_lockout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_VERIFIER_LEN 32
#define PIN_NONCE_HEX 32 /* 16 random bytes -> 32 hex chars */
#define PIN_TOKEN_HEX 32
#define PIN_NONCE_SLOTS 3
#define PIN_NONCE_TTL_MS 30000u
#define PIN_SESSION_TTL_S 120u

typedef enum {
    PIN_OK = 0,
    PIN_ERR_NO_PIN,       /* no PIN has been set yet                                          */
    PIN_ERR_LOCKED,       /* in cooldown, or this failure just started one                     */
    PIN_ERR_BAD_NONCE,    /* unknown / expired / already used: NOT counted as a PIN attempt    */
    PIN_ERR_BAD_RESPONSE, /* wrong PIN: counted                                                */
} pin_verify_result_t;

typedef struct {
    bool has_pin;
    uint8_t verifier[PIN_VERIFIER_LEN];
    struct {
        bool live;
        char hex[PIN_NONCE_HEX + 1];
        uint32_t expires_ms;
    } nonce[PIN_NONCE_SLOTS];
    uint8_t next_slot;
    bool session_valid;
    char token[PIN_TOKEN_HEX + 1];
    uint32_t session_expires_ms;
    retry_lockout_t lockout;
} pin_auth_t;

void pin_auth_init(pin_auth_t *p, retry_lockout_cfg_t lockout_cfg);

/* Install / replace / erase the PIN verifier. Any of these also ends the current session, drops pending nonces and
 * clears the attempt counters (a fresh PIN gets a fresh start). */
void pin_auth_set_verifier(pin_auth_t *p, const uint8_t verifier[PIN_VERIFIER_LEN]);
void pin_auth_clear(pin_auth_t *p);

bool pin_auth_new_challenge(pin_auth_t *p, uint32_t now_ms, const uint8_t rnd[16], char out_hex[PIN_NONCE_HEX + 1]);

/* Verifies one /auth attempt. The nonce is consumed whatever the outcome. On PIN_OK a session token is written to
 * out_token. attempts_remaining / lockout_ms_left are always filled (may be NULL). */
pin_verify_result_t pin_auth_verify(pin_auth_t *p, uint32_t now_ms, const char *nonce_hex, const char *response_hex,
                                    const uint8_t token_rnd[16], char out_token[PIN_TOKEN_HEX + 1],
                                    int *attempts_remaining, uint32_t *lockout_ms_left);

bool pin_auth_session_valid(pin_auth_t *p, uint32_t now_ms, const char *token);
void pin_auth_session_end(pin_auth_t *p);

/* 64 hex chars (either case) -> 32 bytes. False on any other length / non-hex character. */
bool pin_auth_parse_verifier_hex(const char *hex, uint8_t out[PIN_VERIFIER_LEN]);

/* Exposed for tests and for the cross-check against the app's Dart implementation. */
void pin_auth_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void pin_auth_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
