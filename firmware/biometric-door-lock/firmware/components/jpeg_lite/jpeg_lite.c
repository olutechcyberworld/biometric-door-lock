#include "jpeg_lite.h"

#include <math.h>
#include <string.h>

/* Standard Huffman tables (ITU T.81 Annex K), copied from a reference encoder's output. */
static const uint8_t dc_lum_bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t dc_lum_vals[12] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b};
static const uint8_t dc_chr_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t dc_chr_vals[12] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b};
static const uint8_t ac_lum_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125};
static const uint8_t ac_lum_vals[162] = {0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};
static const uint8_t ac_chr_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,119};
static const uint8_t ac_chr_vals[162] = {0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};

/* Annex K quantisation tables, natural (row-major) order. */
static const uint8_t QLUM[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                 14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                 18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
static const uint8_t QCHR[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
static const uint8_t ZZ[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                               12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                               35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                               58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

typedef struct {
    uint16_t code[256];
    uint8_t len[256];
} huff_t;

typedef struct {
    uint8_t *out;
    size_t cap, pos;
    uint32_t acc;
    int nbits;
    int err;
} bw_t;

static void put_byte(bw_t *w, uint8_t b)
{
    if (w->pos >= w->cap) {
        w->err = 1;
        return;
    }
    w->out[w->pos++] = b;
}

static void put_bits(bw_t *w, uint32_t code, int len)
{
    w->acc = (w->acc << len) | (code & ((1u << len) - 1u));
    w->nbits += len;
    while (w->nbits >= 8) {
        uint8_t b = (uint8_t)(w->acc >> (w->nbits - 8));
        put_byte(w, b);
        if (b == 0xFF) {
            put_byte(w, 0x00); /* byte stuffing */
        }
        w->nbits -= 8;
    }
}

static void put_u16(bw_t *w, unsigned v)
{
    put_byte(w, (uint8_t)(v >> 8));
    put_byte(w, (uint8_t)v);
}

static void build_huff(huff_t *h, const uint8_t bits[16], const uint8_t *vals)
{
    memset(h, 0, sizeof(*h));
    unsigned code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++) {
            h->code[vals[k]] = (uint16_t)code;
            h->len[vals[k]] = (uint8_t)l;
            k++;
            code++;
        }
        code <<= 1;
    }
}

static int bitlen(int v)
{
    int n = 0;
    if (v < 0) v = -v;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

static void put_dht(bw_t *w, int tc_th, const uint8_t bits[16], const uint8_t *vals, int nvals)
{
    put_u16(w, 0xFFC4);
    put_u16(w, (unsigned)(2 + 1 + 16 + nvals));
    put_byte(w, (uint8_t)tc_th);
    for (int i = 0; i < 16; i++) put_byte(w, bits[i]);
    for (int i = 0; i < nvals; i++) put_byte(w, vals[i]);
}

static float s_cos[8][8]; /* [u][x] = c(u) * cos((2x+1) u pi / 16), c(0)=1/sqrt(8), c(u>0)=1/2 */
static int s_cos_ready;

static void init_cos(void)
{
    if (s_cos_ready) return;
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++)
            s_cos[u][x] = (float)((u == 0 ? 0.35355339059327373 : 0.5) * cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
    s_cos_ready = 1;
}

/* in: level-shifted samples (row-major). out: quantised coefficients in ZIGZAG order. */
static void fdct_quant(const float in[64], const uint16_t q[64], int16_t out[64])
{
    float tmp[64], c[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int x = 0; x < 8; x++) s += in[y * 8 + x] * s_cos[u][x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            float s = 0;
            for (int y = 0; y < 8; y++) s += tmp[y * 8 + u] * s_cos[v][y];
            c[v * 8 + u] = s;
        }
    for (int k = 0; k < 64; k++) {
        float v = c[ZZ[k]] / (float)q[ZZ[k]];
        out[k] = (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
    }
}

static void put_block(bw_t *w, const int16_t z[64], int *prev_dc, const huff_t *dc, const huff_t *ac)
{
    int diff = z[0] - *prev_dc;
    *prev_dc = z[0];
    int cat = bitlen(diff);
    put_bits(w, dc->code[cat], dc->len[cat]);
    if (cat) put_bits(w, (uint32_t)(diff < 0 ? diff - 1 : diff), cat);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        if (z[k] == 0) {
            run++;
            continue;
        }
        while (run > 15) {
            put_bits(w, ac->code[0xF0], ac->len[0xF0]);
            run -= 16;
        }
        int c = bitlen(z[k]);
        int sym = (run << 4) | c;
        put_bits(w, ac->code[sym], ac->len[sym]);
        put_bits(w, (uint32_t)(z[k] < 0 ? z[k] - 1 : z[k]), c);
        run = 0;
    }
    if (run > 0) put_bits(w, ac->code[0x00], ac->len[0x00]);
}

static void scale_q(const uint8_t *base, int quality, uint16_t out[64])
{
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; i++) {
        int v = (base[i] * scale + 50) / 100;
        out[i] = (uint16_t)(v < 1 ? 1 : (v > 255 ? 255 : v));
    }
}

static void get_rgb(const uint8_t *src, int w, int h, jpeglite_fmt_t fmt, int x, int y, float *r, float *g, float *b)
{
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    if (fmt == JPEGLITE_RGB888) {
        const uint8_t *p = src + ((size_t)y * w + x) * 3;
        *r = p[0]; *g = p[1]; *b = p[2];
        return;
    }
    const uint8_t *p = src + ((size_t)y * w + x) * 2;
    unsigned v = (fmt == JPEGLITE_RGB565_BE) ? (unsigned)((p[0] << 8) | p[1]) : (unsigned)((p[1] << 8) | p[0]);
    unsigned r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
    *r = (float)((r5 << 3) | (r5 >> 2));
    *g = (float)((g6 << 2) | (g6 >> 4));
    *b = (float)((b5 << 3) | (b5 >> 2));
}

int jpeglite_encode(const uint8_t *src, int width, int height, jpeglite_fmt_t fmt, int quality, uint8_t *out,
                    size_t cap)
{
    if (!src || !out || width < 1 || height < 1 || width > 65535 || height > 65535 || quality < 1 || quality > 100) {
        return -1;
    }
    init_cos();
    uint16_t ql[64], qc[64];
    scale_q(QLUM, quality, ql);
    scale_q(QCHR, quality, qc);
    huff_t dcl, dcc, acl, acc_;
    build_huff(&dcl, dc_lum_bits, dc_lum_vals);
    build_huff(&dcc, dc_chr_bits, dc_chr_vals);
    build_huff(&acl, ac_lum_bits, ac_lum_vals);
    build_huff(&acc_, ac_chr_bits, ac_chr_vals);

    bw_t w = {.out = out, .cap = cap};
    put_u16(&w, 0xFFD8); /* SOI */
    put_u16(&w, 0xFFE0); /* JFIF APP0 */
    put_u16(&w, 16);
    const uint8_t jfif[] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    for (size_t i = 0; i < sizeof(jfif); i++) put_byte(&w, jfif[i]);
    for (int t = 0; t < 2; t++) { /* DQT, written in zigzag order */
        put_u16(&w, 0xFFDB);
        put_u16(&w, 67);
        put_byte(&w, (uint8_t)t);
        for (int k = 0; k < 64; k++) put_byte(&w, (uint8_t)(t == 0 ? ql[ZZ[k]] : qc[ZZ[k]]));
    }
    put_u16(&w, 0xFFC0); /* SOF0: baseline, 8-bit, 3 components 1x1 */
    put_u16(&w, 17);
    put_byte(&w, 8);
    put_u16(&w, (unsigned)height);
    put_u16(&w, (unsigned)width);
    put_byte(&w, 3);
    put_byte(&w, 1); put_byte(&w, 0x11); put_byte(&w, 0);
    put_byte(&w, 2); put_byte(&w, 0x11); put_byte(&w, 1);
    put_byte(&w, 3); put_byte(&w, 0x11); put_byte(&w, 1);
    put_dht(&w, 0x00, dc_lum_bits, dc_lum_vals, 12);
    put_dht(&w, 0x10, ac_lum_bits, ac_lum_vals, 162);
    put_dht(&w, 0x01, dc_chr_bits, dc_chr_vals, 12);
    put_dht(&w, 0x11, ac_chr_bits, ac_chr_vals, 162);
    put_u16(&w, 0xFFDA); /* SOS */
    put_u16(&w, 12);
    put_byte(&w, 3);
    put_byte(&w, 1); put_byte(&w, 0x00);
    put_byte(&w, 2); put_byte(&w, 0x11);
    put_byte(&w, 3); put_byte(&w, 0x11);
    put_byte(&w, 0); put_byte(&w, 63); put_byte(&w, 0);

    int pdy = 0, pdb = 0, pdr = 0;
    for (int by = 0; by < height; by += 8) {
        for (int bx = 0; bx < width; bx += 8) {
            float Y[64], Cb[64], Cr[64];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    float r, g, b;
                    get_rgb(src, width, height, fmt, bx + x, by + y, &r, &g, &b);
                    Y[y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.f;
                    Cb[y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    Cr[y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            int16_t z[64];
            fdct_quant(Y, ql, z);
            put_block(&w, z, &pdy, &dcl, &acl);
            fdct_quant(Cb, qc, z);
            put_block(&w, z, &pdb, &dcc, &acc_);
            fdct_quant(Cr, qc, z);
            put_block(&w, z, &pdr, &dcc, &acc_);
            if (w.err) return -1;
        }
    }
    if (w.nbits > 0) put_bits(&w, 0x7F, 8 - w.nbits); /* pad the last byte with 1s */
    put_u16(&w, 0xFFD9);                              /* EOI */
    return w.err ? -1 : (int)w.pos;
}
