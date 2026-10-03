#include "as608.h"

#include <string.h>

#define HDR_HI 0xEF
#define HDR_LO 0x01
#define PID_CMD 0x01
#define PID_ACK 0x07

/* command bytes */
#define CMD_GET_IMAGE 0x01
#define CMD_IMG2TZ 0x02
#define CMD_REG_MODEL 0x05
#define CMD_STORE 0x06
#define CMD_DELETE 0x0C
#define CMD_EMPTY 0x0D
#define CMD_SET_SYSPARA 0x0E
#define CMD_READ_SYSPARA 0x0F
#define CMD_SEARCH 0x04
#define CMD_VFY_PWD 0x13
#define CMD_TEMPLATE_NUM 0x1D
#define CMD_READ_INDEX 0x1F

#define T_SHORT 600u   /* ms: ordinary command                              */
#define T_SEARCH 2500u /* ms: 1:N search over a full library                */
#define T_SLOW 3000u   /* ms: empty library / flash writes                  */

static uint16_t sum16(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) {
        s += p[i];
    }
    return (uint16_t)s;
}

void as608_init(as608_t *dev, as608_transport_t io, uint32_t address, uint32_t password)
{
    memset(dev, 0, sizeof(*dev));
    dev->io = io;
    dev->address = address;
    dev->password = password;
    dev->capacity = 162; /* replaced by as608_read_params() */
}

const char *as608_strerror(int code)
{
    switch (code) {
    case AS608_OK: return "ok";
    case AS608_ERR_COMM: return "no response from module (check TX/RX wiring; 5 V power; baud; GPIO pins)";
    case AS608_ERR_CHECKSUM: return "corrupt response (wrong baud or electrical noise)";
    case AS608_ERR_TIMEOUT: return "timed out waiting for a finger";
    case AS608_ERR_ARG: return "bad argument";
    case AS608_ERR_NO_SPACE: return "fingerprint library is full";
    case 0x01: return "module: packet receive error";
    case 0x02: return "module: no finger on the sensor";
    case 0x03: return "module: image capture failed";
    case 0x06: return "module: image too messy (dry or wet finger; dirty glass)";
    case 0x07: return "module: too few feature points (press more firmly / more of the finger)";
    case 0x08: return "module: the two captures do not match";
    case 0x09: return "module: no match in library";
    case 0x0A: return "module: could not merge the two captures (use the same finger both times)";
    case 0x0B: return "module: id out of range";
    case 0x10: return "module: delete failed";
    case 0x11: return "module: library clear failed";
    case 0x13: return "module: wrong password";
    case 0x18: return "module: flash write error";
    default: return "module: error code";
    }
}

static int send_cmd(as608_t *d, const uint8_t *payload, size_t n)
{
    uint8_t f[32];
    if (n + 11 > sizeof f) {
        return AS608_ERR_ARG;
    }
    size_t i = 0;
    f[i++] = HDR_HI;
    f[i++] = HDR_LO;
    f[i++] = (uint8_t)(d->address >> 24);
    f[i++] = (uint8_t)(d->address >> 16);
    f[i++] = (uint8_t)(d->address >> 8);
    f[i++] = (uint8_t)(d->address);
    f[i++] = PID_CMD;
    uint16_t len = (uint16_t)(n + 2); /* payload + checksum */
    f[i++] = (uint8_t)(len >> 8);
    f[i++] = (uint8_t)(len);
    memcpy(&f[i], payload, n);
    i += n;
    uint16_t cs = sum16(&f[6], i - 6); /* PID + length + payload */
    f[i++] = (uint8_t)(cs >> 8);
    f[i++] = (uint8_t)(cs);
    return d->io.write(f, i, d->io.ctx) == (int)i ? 0 : AS608_ERR_COMM;
}

/* Reads exactly n bytes within the time budget that remains since t0. Returns bytes read. */
static int read_exact(as608_t *d, uint8_t *buf, size_t n, uint32_t t0, uint32_t timeout_ms)
{
    size_t got = 0;
    while (got < n) {
        uint32_t el = d->io.now_ms(d->io.ctx) - t0;
        if (el >= timeout_ms) {
            break;
        }
        int r = d->io.read(buf + got, n - got, timeout_ms - el, d->io.ctx);
        if (r < 0) {
            return r;
        }
        got += (size_t)r;
    }
    return (int)got;
}

/* Returns payload length (>=1) on success, negative local error otherwise. */
static int recv_ack(as608_t *d, uint8_t *payload, size_t max, uint32_t timeout_ms)
{
    uint32_t t0 = d->io.now_ms(d->io.ctx);
    int state = 0;
    uint8_t b;
    d->dbg_rx = 0;
    d->dbg_stage = 0;
    while (state < 2) { /* resynchronise on EF 01 */
        int r = read_exact(d, &b, 1, t0, timeout_ms);
        if (r <= 0) {
            return AS608_ERR_COMM; /* dbg_stage stays 0 (silence) or 1 (only noise arrived) */
        }
        d->dbg_rx++;
        d->dbg_stage = 1;
        if (state == 0) {
            state = (b == HDR_HI) ? 1 : 0;
        } else {
            state = (b == HDR_LO) ? 2 : ((b == HDR_HI) ? 1 : 0);
        }
    }
    d->dbg_rx = (uint16_t)(d->dbg_rx);
    uint8_t h[7]; /* address(4) pid(1) length(2) */
    int got = read_exact(d, h, sizeof h, t0, timeout_ms);
    if (got > 0) {
        d->dbg_rx = (uint16_t)(d->dbg_rx + got);
    }
    if (got != (int)sizeof h) {
        d->dbg_stage = 3;
        return AS608_ERR_COMM;
    }
    uint32_t addr = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    uint16_t len = (uint16_t)((h[5] << 8) | h[6]);
    if (addr != d->address || h[4] != PID_ACK || len < 3 || (size_t)(len - 2) > max) {
        d->dbg_stage = 2;
        return AS608_ERR_COMM;
    }
    uint8_t tail[2];
    int gp = read_exact(d, payload, (size_t)len - 2, t0, timeout_ms);
    int gt = (gp == (int)(len - 2)) ? read_exact(d, tail, 2, t0, timeout_ms) : 0;
    d->dbg_rx = (uint16_t)(d->dbg_rx + (gp > 0 ? gp : 0) + (gt > 0 ? gt : 0));
    if (gp != (int)(len - 2) || gt != 2) {
        d->dbg_stage = 3;
        return AS608_ERR_COMM;
    }
    uint32_t cs = (uint32_t)h[4] + h[5] + h[6] + sum16(payload, (size_t)len - 2);
    if ((uint16_t)cs != (uint16_t)((tail[0] << 8) | tail[1])) {
        d->dbg_stage = 4;
        return AS608_ERR_CHECKSUM;
    }
    d->dbg_stage = 5;
    return (int)(len - 2);
}

/* One command/response exchange. Returns the module confirmation code (0 = ok) or a negative local error. */
static int xfer(as608_t *d, const uint8_t *cmd, size_t n, uint8_t *resp, size_t max, size_t *rn, uint32_t timeout_ms)
{
    if (d->io.flush_rx) {
        d->io.flush_rx(d->io.ctx);
    }
    int r = send_cmd(d, cmd, n);
    if (r < 0) {
        return r;
    }
    r = recv_ack(d, resp, max, timeout_ms);
    if (r < 0) {
        return r;
    }
    if (rn) {
        *rn = (size_t)r;
    }
    return resp[0];
}

int as608_verify_password(as608_t *d)
{
    uint8_t c[5] = {CMD_VFY_PWD, (uint8_t)(d->password >> 24), (uint8_t)(d->password >> 16),
                    (uint8_t)(d->password >> 8), (uint8_t)d->password};
    uint8_t r[4];
    return xfer(d, c, sizeof c, r, sizeof r, NULL, T_SHORT);
}

int as608_read_params(as608_t *d, as608_params_t *o)
{
    uint8_t c[1] = {CMD_READ_SYSPARA};
    uint8_t r[20];
    size_t n = 0;
    int code = xfer(d, c, 1, r, sizeof r, &n, T_SHORT);
    if (code != AS608_OK) {
        return code;
    }
    if (n < 17) {
        return AS608_ERR_COMM;
    }
    o->status_register = (uint16_t)((r[1] << 8) | r[2]);
    o->system_id = (uint16_t)((r[3] << 8) | r[4]);
    o->capacity = (uint16_t)((r[5] << 8) | r[6]);
    o->security_level = (uint16_t)((r[7] << 8) | r[8]);
    o->device_address = ((uint32_t)r[9] << 24) | ((uint32_t)r[10] << 16) | ((uint32_t)r[11] << 8) | r[12];
    o->packet_size_code = (uint16_t)((r[13] << 8) | r[14]);
    o->baud_multiplier = (uint16_t)((r[15] << 8) | r[16]);
    if (o->capacity > 0) {
        d->capacity = o->capacity;
    }
    return AS608_OK;
}

int as608_template_count(as608_t *d, uint16_t *count)
{
    uint8_t c[1] = {CMD_TEMPLATE_NUM};
    uint8_t r[4];
    size_t n = 0;
    int code = xfer(d, c, 1, r, sizeof r, &n, T_SHORT);
    if (code != AS608_OK) {
        return code;
    }
    if (n < 3) {
        return AS608_ERR_COMM;
    }
    *count = (uint16_t)((r[1] << 8) | r[2]);
    return AS608_OK;
}

int as608_set_security_level(as608_t *d, uint8_t level)
{
    if (level < 1 || level > 5) {
        return AS608_ERR_ARG;
    }
    uint8_t c[3] = {CMD_SET_SYSPARA, 5, level};
    uint8_t r[4];
    return xfer(d, c, sizeof c, r, sizeof r, NULL, T_SHORT);
}

int as608_get_image(as608_t *d)
{
    uint8_t c[1] = {CMD_GET_IMAGE};
    uint8_t r[4];
    return xfer(d, c, 1, r, sizeof r, NULL, T_SHORT);
}

int as608_image_to_char(as608_t *d, uint8_t buffer)
{
    if (buffer != 1 && buffer != 2) {
        return AS608_ERR_ARG;
    }
    uint8_t c[2] = {CMD_IMG2TZ, buffer};
    uint8_t r[4];
    return xfer(d, c, sizeof c, r, sizeof r, NULL, T_SHORT);
}

int as608_reg_model(as608_t *d)
{
    uint8_t c[1] = {CMD_REG_MODEL};
    uint8_t r[4];
    return xfer(d, c, 1, r, sizeof r, NULL, T_SHORT);
}

int as608_store(as608_t *d, uint8_t buffer, uint16_t id)
{
    uint8_t c[4] = {CMD_STORE, buffer, (uint8_t)(id >> 8), (uint8_t)id};
    uint8_t r[4];
    return xfer(d, c, sizeof c, r, sizeof r, NULL, T_SLOW);
}

int as608_search(as608_t *d, uint8_t buffer, uint16_t start, uint16_t count, uint16_t *id, uint16_t *score)
{
    uint8_t c[6] = {CMD_SEARCH, buffer, (uint8_t)(start >> 8), (uint8_t)start, (uint8_t)(count >> 8), (uint8_t)count};
    uint8_t r[8];
    size_t n = 0;
    int code = xfer(d, c, sizeof c, r, sizeof r, &n, T_SEARCH);
    if (code != AS608_OK) {
        return code;
    }
    if (n < 5) {
        return AS608_ERR_COMM;
    }
    if (id) {
        *id = (uint16_t)((r[1] << 8) | r[2]);
    }
    if (score) {
        *score = (uint16_t)((r[3] << 8) | r[4]);
    }
    return AS608_OK;
}

int as608_delete(as608_t *d, uint16_t id, uint16_t count)
{
    uint8_t c[5] = {CMD_DELETE, (uint8_t)(id >> 8), (uint8_t)id, (uint8_t)(count >> 8), (uint8_t)count};
    uint8_t r[4];
    return xfer(d, c, sizeof c, r, sizeof r, NULL, T_SLOW);
}

int as608_empty(as608_t *d)
{
    uint8_t c[1] = {CMD_EMPTY};
    uint8_t r[4];
    return xfer(d, c, 1, r, sizeof r, NULL, T_SLOW);
}

int as608_read_index_table(as608_t *d, uint8_t page, uint8_t out[32])
{
    uint8_t c[2] = {CMD_READ_INDEX, page};
    uint8_t r[40];
    size_t n = 0;
    int code = xfer(d, c, sizeof c, r, sizeof r, &n, T_SHORT);
    if (code != AS608_OK) {
        return code;
    }
    if (n < 33) {
        return AS608_ERR_COMM;
    }
    memcpy(out, &r[1], 32);
    return AS608_OK;
}

int as608_capture(as608_t *d, uint8_t buffer, uint32_t timeout_ms, uint32_t *wait_ms, uint32_t *capture_ms)
{
    uint32_t t0 = d->io.now_ms(d->io.ctx);
    int comm_errors = 0;
    for (;;) {
        int r = as608_get_image(d);
        if (r == AS608_OK) {
            break;
        }
        if (r < 0) {
            if (++comm_errors >= 3) {
                return r; /* not just a glitch */
            }
        } else if (r != AS608_NO_FINGER && r != AS608_IMAGE_FAIL) {
            return r;
        }
        if (d->io.now_ms(d->io.ctx) - t0 >= timeout_ms) {
            return AS608_ERR_TIMEOUT;
        }
        d->io.delay_ms(60, d->io.ctx);
    }
    uint32_t t1 = d->io.now_ms(d->io.ctx);
    if (wait_ms) {
        *wait_ms = t1 - t0;
    }
    int r = as608_image_to_char(d, buffer);
    if (capture_ms) {
        *capture_ms = d->io.now_ms(d->io.ctx) - t1;
    }
    return r;
}

int as608_identify(as608_t *d, uint32_t timeout_ms, as608_identify_t *o)
{
    memset(o, 0, sizeof(*o));
    int r = as608_capture(d, 1, timeout_ms, &o->wait_ms, &o->capture_ms);
    if (r != AS608_OK) {
        return r;
    }
    uint32_t t = d->io.now_ms(d->io.ctx);
    r = as608_search(d, 1, 0, d->capacity, &o->id, &o->score);
    o->search_ms = d->io.now_ms(d->io.ctx) - t;
    return r; /* AS608_NOT_FOUND (0x09) = valid finger, not enrolled */
}

int as608_enroll(as608_t *d, uint16_t id, uint32_t timeout_ms, as608_enroll_cb cb, void *user)
{
    if (id >= d->capacity) {
        return AS608_ERR_ARG;
    }
    int r;
    if (cb) cb(AS608_STEP_PLACE1, user);
    r = as608_capture(d, 1, timeout_ms, NULL, NULL);
    if (r != AS608_OK) {
        return r;
    }
    if (cb) cb(AS608_STEP_REMOVE, user);
    uint32_t t0 = d->io.now_ms(d->io.ctx);
    int absent = 0;
    while (absent < 3) { /* three consecutive "no finger" reads = really lifted */
        int g = as608_get_image(d);
        absent = (g == AS608_NO_FINGER) ? absent + 1 : 0;
        if (d->io.now_ms(d->io.ctx) - t0 >= timeout_ms) {
            return AS608_ERR_TIMEOUT;
        }
        d->io.delay_ms(80, d->io.ctx);
    }
    if (cb) cb(AS608_STEP_PLACE2, user);
    r = as608_capture(d, 2, timeout_ms, NULL, NULL);
    if (r != AS608_OK) {
        return r;
    }
    r = as608_reg_model(d);
    if (r != AS608_OK) {
        return r;
    }
    if (cb) cb(AS608_STEP_STORING, user);
    return as608_store(d, 1, id);
}

int as608_next_free_id(as608_t *d, uint16_t *id, uint16_t *used_count)
{
    uint16_t used = 0;
    int have_free = 0;
    uint16_t first_free = 0;
    for (uint8_t page = 0; (uint16_t)page * 256u < d->capacity; page++) {
        uint8_t bits[32];
        int r = as608_read_index_table(d, page, bits);
        if (r != AS608_OK) {
            return r;
        }
        for (uint16_t k = 0; k < 256u; k++) {
            uint16_t idx = (uint16_t)(page * 256u + k);
            if (idx >= d->capacity) {
                break;
            }
            if (bits[k / 8] & (1u << (k % 8))) {
                used++;
            } else if (!have_free) {
                have_free = 1;
                first_free = idx;
            }
        }
    }
    if (used_count) {
        *used_count = used;
    }
    if (!have_free) {
        return AS608_ERR_NO_SPACE;
    }
    *id = first_free;
    return AS608_OK;
}
