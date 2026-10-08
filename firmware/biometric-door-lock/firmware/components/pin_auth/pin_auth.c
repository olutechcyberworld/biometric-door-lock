#include "pin_auth.h"

#include <string.h>

/* ---------- SHA-256 / HMAC-SHA256 (FIPS 180-4 / RFC 2104), small and dependency free ---------- */

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t total;
    size_t fill;
} sha256_ctx;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) |
               (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv));
    c->total = 0;
    c->fill = 0;
}

static void sha256_update(sha256_ctx *c, const uint8_t *d, size_t n)
{
    c->total += n;
    while (n) {
        size_t take = 64 - c->fill;
        if (take > n) {
            take = n;
        }
        memcpy(c->buf + c->fill, d, take);
        c->fill += take;
        d += take;
        n -= take;
        if (c->fill == 64) {
            sha256_block(c, c->buf);
            c->fill = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->fill != 56) {
        sha256_update(c, &zero, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void pin_auth_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void pin_auth_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    uint8_t k[64] = {0};
    if (key_len > 64) {
        pin_auth_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    uint8_t inner[32];
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, msg, msg_len);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* ---------- hex helpers ---------- */

static const char HEXCH[] = "0123456789abcdef";

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = HEXCH[in[i] >> 4];
        out[2 * i + 1] = HEXCH[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool pin_auth_parse_verifier_hex(const char *hex, uint8_t out[PIN_VERIFIER_LEN])
{
    if (!hex || strlen(hex) != 2 * PIN_VERIFIER_LEN) {
        return false;
    }
    for (int i = 0; i < PIN_VERIFIER_LEN; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* Compares the LOWERCASED response with the expected hex in constant time (no early exit on the first difference). */
static bool hex_equal_ct(const char *resp, const char *expected, size_t n)
{
    if (!resp || strlen(resp) != n) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        char c = resp[i];
        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        }
        diff |= (uint8_t)(c ^ expected[i]);
    }
    return diff == 0;
}

/* ---------- state machine ---------- */

void pin_auth_init(pin_auth_t *p, retry_lockout_cfg_t cfg)
{
    memset(p, 0, sizeof(*p));
    retry_lockout_init(&p->lockout, cfg);
}

static void reset_volatile(pin_auth_t *p)
{
    memset(p->nonce, 0, sizeof(p->nonce));
    p->next_slot = 0;
    p->session_valid = false;
    memset(p->token, 0, sizeof(p->token));
    retry_lockout_cfg_t cfg = p->lockout.cfg;
    retry_lockout_init(&p->lockout, cfg);
}

void pin_auth_set_verifier(pin_auth_t *p, const uint8_t verifier[PIN_VERIFIER_LEN])
{
    memcpy(p->verifier, verifier, PIN_VERIFIER_LEN);
    p->has_pin = true;
    reset_volatile(p);
}

void pin_auth_clear(pin_auth_t *p)
{
    memset(p->verifier, 0, sizeof(p->verifier));
    p->has_pin = false;
    reset_volatile(p);
}

bool pin_auth_new_challenge(pin_auth_t *p, uint32_t now_ms, const uint8_t rnd[16], char out_hex[PIN_NONCE_HEX + 1])
{
    to_hex(rnd, 16, out_hex);
    uint8_t s = p->next_slot;
    p->nonce[s].live = true;
    memcpy(p->nonce[s].hex, out_hex, PIN_NONCE_HEX + 1);
    p->nonce[s].expires_ms = now_ms + PIN_NONCE_TTL_MS;
    p->next_slot = (uint8_t)((s + 1) % PIN_NONCE_SLOTS);
    return true;
}

static int attempts_left(const pin_auth_t *p)
{
    int left = (int)p->lockout.cfg.max_attempts - (int)p->lockout.fail_count;
    return left < 0 ? 0 : left;
}

pin_verify_result_t pin_auth_verify(pin_auth_t *p, uint32_t now_ms, const char *nonce_hex, const char *response_hex,
                                    const uint8_t token_rnd[16], char out_token[PIN_TOKEN_HEX + 1],
                                    int *attempts_remaining, uint32_t *lockout_ms_left)
{
    pin_verify_result_t res;
    if (lockout_ms_left) *lockout_ms_left = 0;
    if (!p->has_pin) {
        if (attempts_remaining) *attempts_remaining = 0;
        return PIN_ERR_NO_PIN;
    }
    if (retry_lockout_is_locked(&p->lockout, now_ms)) {
        if (attempts_remaining) *attempts_remaining = 0;
        if (lockout_ms_left) *lockout_ms_left = p->lockout.unlock_at_ms - now_ms;
        return PIN_ERR_LOCKED;
    }

    int slot = -1;
    if (nonce_hex) {
        for (int i = 0; i < PIN_NONCE_SLOTS; i++) {
            if (p->nonce[i].live && (int32_t)(now_ms - p->nonce[i].expires_ms) < 0 &&
                strcmp(p->nonce[i].hex, nonce_hex) == 0) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        if (attempts_remaining) *attempts_remaining = attempts_left(p);
        return PIN_ERR_BAD_NONCE;
    }
    p->nonce[slot].live = false; /* single use, whatever happens next */

    uint8_t mac[32];
    char expected[65];
    pin_auth_hmac_sha256(p->verifier, PIN_VERIFIER_LEN, (const uint8_t *)p->nonce[slot].hex, PIN_NONCE_HEX, mac);
    to_hex(mac, 32, expected);

    if (hex_equal_ct(response_hex, expected, 64)) {
        retry_lockout_on_success(&p->lockout);
        to_hex(token_rnd, 16, out_token);
        memcpy(p->token, out_token, PIN_TOKEN_HEX + 1);
        p->session_valid = true;
        p->session_expires_ms = now_ms + PIN_SESSION_TTL_S * 1000u;
        if (attempts_remaining) *attempts_remaining = (int)p->lockout.cfg.max_attempts;
        return PIN_OK;
    }

    res = PIN_ERR_BAD_RESPONSE;
    if (retry_lockout_on_fail(&p->lockout, now_ms) == RETRY_ACT_LOCKOUT) {
        res = PIN_ERR_LOCKED;
        if (lockout_ms_left) *lockout_ms_left = p->lockout.unlock_at_ms - now_ms;
    }
    if (attempts_remaining) *attempts_remaining = attempts_left(p);
    return res;
}

bool pin_auth_session_valid(pin_auth_t *p, uint32_t now_ms, const char *token)
{
    if (!p->session_valid || !token) {
        return false;
    }
    if ((int32_t)(now_ms - p->session_expires_ms) >= 0) {
        p->session_valid = false;
        return false;
    }
    if (strlen(token) != PIN_TOKEN_HEX) {
        return false;
    }
    uint8_t diff = 0;
    for (int i = 0; i < PIN_TOKEN_HEX; i++) {
        diff |= (uint8_t)(token[i] ^ p->token[i]);
    }
    return diff == 0;
}

void pin_auth_session_end(pin_auth_t *p)
{
    p->session_valid = false;
    memset(p->token, 0, sizeof(p->token));
}
