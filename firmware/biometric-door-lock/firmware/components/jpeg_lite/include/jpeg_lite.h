#pragma once
/* Tiny baseline JPEG encoder (YCbCr 4:4:4, standard Huffman tables) for the live enrollment preview.
 * Pure C, no ESP-IDF, so it is host-tested and has no component-registry dependency to go stale. Not fast, not
 * pretty: a 240x240 preview at quality ~40 is about 8 KB and a few tens of milliseconds on the ESP32-S3. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { JPEGLITE_RGB565_BE = 0, JPEGLITE_RGB565_LE, JPEGLITE_RGB888 } jpeglite_fmt_t;

/* Returns the number of bytes written to out, or -1 if cap is too small / arguments are invalid.
 * quality 1..100 (IJG scaling). Any width/height >= 1 works (edges are replicated to fill 8x8 blocks). */
int jpeglite_encode(const uint8_t *src, int width, int height, jpeglite_fmt_t fmt, int quality, uint8_t *out,
                    size_t cap);

#ifdef __cplusplus
}
#endif
