// Host test for pin_auth. No ESP-IDF - build/run per tests/run_host_tests.sh.
// Vectors marked "python" were produced with hashlib/hmac, the same construction lib/services/crypto_service.dart uses.
#include <cstdio>
#include <cstring>
#include "pin_auth.h"

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

static void hex(const uint8_t *b, size_t n, char *o)
{
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { o[2 * i] = H[b[i] >> 4]; o[2 * i + 1] = H[b[i] & 15]; }
    o[2 * n] = 0;
}

static void rnd(uint8_t out[16], uint8_t seed) { for (int i = 0; i < 16; i++) out[i] = (uint8_t)(seed + i); }

// What the app does: key = SHA256(pin), message = nonce hex TEXT.
static void app_response(const char *pin, const char *nonce, char out[65])
{
    uint8_t k[32], mac[32];
    pin_auth_sha256((const uint8_t *)pin, strlen(pin), k);
    pin_auth_hmac_sha256(k, 32, (const uint8_t *)nonce, strlen(nonce), mac);
    hex(mac, 32, out);
}

static void set_pin(pin_auth_t *p, const char *pin)
{
    uint8_t k[32];
    pin_auth_sha256((const uint8_t *)pin, strlen(pin), k);
    pin_auth_set_verifier(p, k);
}

int main()
{
    char h[65];
    uint8_t d[32];

    // --- primitives ---
    pin_auth_sha256((const uint8_t *)"abc", 3, d); hex(d, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")); // FIPS 180-2
    pin_auth_sha256((const uint8_t *)"123456", 6, d); hex(d, 32, h);
    CHECK(!strcmp(h, "8d969eef6ecad3c29a3a629280e686cf0c3f5d5a86aff3ca12020c923adc6c92")); // python
    pin_auth_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")); // RFC 4231 case 2
    // The exact contract vector: nonce hex TEXT, not decoded bytes.
    app_response("123456", "00112233445566778899aabbccddeeff", h);
    CHECK(!strcmp(h, "e7697db408fba588ffe81874f0262c5f3ca97d93d08206f48141e2cebfeee9d3")); // python
    CHECK(strcmp(h, "df5808470685b5deb8b58ce2401e32f504804662a03a504b995a600c07a54944") != 0); // decoded-bytes variant

    // --- verifier hex parsing ---
    CHECK(pin_auth_parse_verifier_hex("8d969eef6ecad3c29a3a629280e686cf0c3f5d5a86aff3ca12020c923adc6c92", d));
    CHECK(d[0] == 0x8d && d[31] == 0x92);
    CHECK(pin_auth_parse_verifier_hex("8D969EEF6ECAD3C29A3A629280E686CF0C3F5D5A86AFF3CA12020C923ADC6C92", d));
    CHECK(!pin_auth_parse_verifier_hex("8d96", d));
    CHECK(!pin_auth_parse_verifier_hex("zz969eef6ecad3c29a3a629280e686cf0c3f5d5a86aff3ca12020c923adc6c92", d));
    CHECK(!pin_auth_parse_verifier_hex(nullptr, d));

    // --- no PIN yet ---
    {
        pin_auth_t p; pin_auth_init(&p, cfg());
        char tok[33]; int left = -1; uint32_t ms = 1;
        CHECK(pin_auth_verify(&p, 0, "x", "y", d, tok, &left, &ms) == PIN_ERR_NO_PIN);
    }

    // --- happy path + single-use nonce + session ---
    {
        pin_auth_t p; pin_auth_init(&p, cfg()); set_pin(&p, "123456");
        uint8_t r[16], t[16]; rnd(r, 0); rnd(t, 100);
        char nonce[33], tok[33], resp[65]; int left = 0; uint32_t ms = 0;
        CHECK(pin_auth_new_challenge(&p, 1000, r, nonce));
        CHECK(!strcmp(nonce, "000102030405060708090a0b0c0d0e0f"));
        app_response("123456", nonce, resp);
        CHECK(pin_auth_verify(&p, 2000, nonce, resp, t, tok, &left, &ms) == PIN_OK);
        CHECK(!strcmp(tok, "6465666768696a6b6c6d6e6f70717273"));
        CHECK(pin_auth_session_valid(&p, 2000, tok));
        CHECK(!pin_auth_session_valid(&p, 2000, "00000000000000000000000000000000"));
        CHECK(pin_auth_session_valid(&p, 2000 + 119000, tok));
        CHECK(!pin_auth_session_valid(&p, 2000 + 120000, tok)); // expires_in = 120 s
        // same nonce again: consumed
        CHECK(pin_auth_verify(&p, 2100, nonce, resp, t, tok, &left, &ms) == PIN_ERR_BAD_NONCE);
        // uppercase response is accepted (hex is case-insensitive), wrong length is not
        pin_auth_new_challenge(&p, 3000, r, nonce);
        app_response("123456", nonce, resp);
        char up[65]; for (int i = 0; i < 65; i++) up[i] = (resp[i] >= 'a' && resp[i] <= 'f') ? resp[i] - 32 : resp[i];
        CHECK(pin_auth_verify(&p, 3100, nonce, up, t, tok, &left, &ms) == PIN_OK);
        pin_auth_new_challenge(&p, 3200, r, nonce);
        CHECK(pin_auth_verify(&p, 3300, nonce, "abcd", t, tok, &left, &ms) == PIN_ERR_BAD_RESPONSE);
    }

    // --- nonce expiry (30 s) ---
    {
        pin_auth_t p; pin_auth_init(&p, cfg()); set_pin(&p, "123456");
        uint8_t r[16], t[16]; rnd(r, 7); rnd(t, 9);
        char nonce[33], tok[33], resp[65]; int left; uint32_t ms;
        pin_auth_new_challenge(&p, 0, r, nonce);
        app_response("123456", nonce, resp);
        CHECK(pin_auth_verify(&p, 30000, nonce, resp, t, tok, &left, &ms) == PIN_ERR_BAD_NONCE);
        pin_auth_new_challenge(&p, 0, r, nonce);
        CHECK(pin_auth_verify(&p, 29999, nonce, resp, t, tok, &left, &ms) == PIN_OK);
    }

    // --- wrong PIN: attempts_remaining counts down, then lockout, bad nonces are NOT attempts ---
    {
        pin_auth_t p; pin_auth_init(&p, cfg()); set_pin(&p, "123456");
        uint8_t r[16], t[16]; char nonce[33], tok[33], resp[65]; int left; uint32_t ms;
        rnd(r, 1); rnd(t, 2);
        pin_auth_new_challenge(&p, 0, r, nonce); app_response("000000", nonce, resp);
        CHECK(pin_auth_verify(&p, 10, nonce, resp, t, tok, &left, &ms) == PIN_ERR_BAD_RESPONSE && left == 2);
        CHECK(pin_auth_verify(&p, 11, "deadbeef", resp, t, tok, &left, &ms) == PIN_ERR_BAD_NONCE && left == 2);
        rnd(r, 2); pin_auth_new_challenge(&p, 20, r, nonce); app_response("000000", nonce, resp);
        CHECK(pin_auth_verify(&p, 30, nonce, resp, t, tok, &left, &ms) == PIN_ERR_BAD_RESPONSE && left == 1);
        rnd(r, 3); pin_auth_new_challenge(&p, 40, r, nonce); app_response("000000", nonce, resp);
        CHECK(pin_auth_verify(&p, 50, nonce, resp, t, tok, &left, &ms) == PIN_ERR_LOCKED && left == 0 && ms == 30000);
        // while locked even the RIGHT pin is refused, and it does not burn the nonce
        rnd(r, 4); pin_auth_new_challenge(&p, 60, r, nonce); app_response("123456", nonce, resp);
        CHECK(pin_auth_verify(&p, 1000, nonce, resp, t, tok, &left, &ms) == PIN_ERR_LOCKED && ms == 29050);
        // cooldown served exactly at 30050; the same (unburnt, still-fresh) nonce now verifies
        CHECK(pin_auth_verify(&p, 30050, nonce, resp, t, tok, &left, &ms) == PIN_OK);
    }

    // --- changing / clearing the PIN drops the session and the old PIN stops working ---
    {
        pin_auth_t p; pin_auth_init(&p, cfg()); set_pin(&p, "123456");
        uint8_t r[16], t[16]; char nonce[33], tok[33], resp[65]; int left; uint32_t ms;
        rnd(r, 11); rnd(t, 12);
        pin_auth_new_challenge(&p, 0, r, nonce); app_response("123456", nonce, resp);
        CHECK(pin_auth_verify(&p, 1, nonce, resp, t, tok, &left, &ms) == PIN_OK);
        CHECK(pin_auth_session_valid(&p, 2, tok));
        set_pin(&p, "654321");
        CHECK(!pin_auth_session_valid(&p, 3, tok));
        pin_auth_new_challenge(&p, 4, r, nonce); app_response("123456", nonce, resp);
        CHECK(pin_auth_verify(&p, 5, nonce, resp, t, tok, &left, &ms) == PIN_ERR_BAD_RESPONSE);
        pin_auth_new_challenge(&p, 6, r, nonce); app_response("654321", nonce, resp);
        CHECK(pin_auth_verify(&p, 7, nonce, resp, t, tok, &left, &ms) == PIN_OK);
        pin_auth_clear(&p);
        CHECK(!pin_auth_session_valid(&p, 8, tok));
        CHECK(pin_auth_verify(&p, 9, nonce, resp, t, tok, &left, &ms) == PIN_ERR_NO_PIN);
    }

    printf("pin_auth: all tests passed\n");
    return 0;
}
