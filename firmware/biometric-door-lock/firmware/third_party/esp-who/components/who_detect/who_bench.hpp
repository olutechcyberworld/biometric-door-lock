#pragma once
// Phase 1 benchmark helpers: microsecond timing + one CSV line per recognition attempt.
// BENCH,seq,faces,detect_us,recog_us,e2e_us,id,sim
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include "dl_image_define.hpp"
#include "dl_detect_define.hpp"
#include "esp_timer.h"
#include "who_cam_counter.hpp"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace who {
namespace bench {
inline std::atomic<int64_t> g_trigger_us{0}; // set when 'r' is received on the console
inline std::atomic<int64_t> g_detect_us{0};  // duration of the most recent detect model run
inline std::atomic<uint32_t> g_seq{0};
inline std::atomic<uint32_t> g_frames{0}; // frames the detector has processed (proves the camera is streaming)

inline void arm()
{
    g_trigger_us.store(esp_timer_get_time());
}

// Heartbeat: lets the dashboard see uptime + whether frames are actually flowing, without any dump request.
inline void heartbeat()
{
    std::printf("HB,%" PRId64 ",%u,%" PRId64 ",%u\n", esp_timer_get_time() / 1000, (unsigned)g_frames.load(),
                g_detect_us.load(), (unsigned)g_cam_frames.load());
}

inline void report(size_t faces, int64_t recog_start_us, int64_t recog_end_us, int id, float sim)
{
    int64_t trig = g_trigger_us.load();
    int64_t e2e = trig ? (recog_end_us - trig) : -1;
    std::printf("BENCH,%" PRIu32 ",%u,%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%.3f\n",
                g_seq.fetch_add(1),
                (unsigned)faces,
                g_detect_us.load(),
                recog_end_us - recog_start_us,
                e2e,
                id,
                (double)sim);
}

// ---------------------------------------------------------------------------------------------
// Frame tap: dump the exact image handed to the detector (plus its detections) as text lines.
//   FRAME_BEGIN,w,h,pixname,nbytes,step,pixtype_enum
//   BOX,x1,y1,x2,y2,score_x1000[,kp1x,kp1y,...]     (coordinates in ORIGINAL frame pixels)
//   D,<base64 of up to 72 raw bytes>                  (one self-contained line each)
//   FRAME_END
// Text/base64 on purpose: survives CRLF translation on the UART console and interleaved log lines.
// ---------------------------------------------------------------------------------------------
// Optional per-frame hook (main/session_ws.cpp): called in the detector task for EVERY frame with the exact image
// the detector saw and its detections. Must be cheap and must not block. nullptr = off.
typedef void (*frame_tap_fn)(const dl::image::img_t &img, const std::list<dl::detect::result_t> &res);
inline frame_tap_fn g_frame_tap = nullptr;

enum { DUMP_NONE = 0, DUMP_FULL = 1, DUMP_HALF = 2, DUMP_QUARTER = 3 };
inline std::atomic<int> g_dump_req{DUMP_NONE};

inline const char *pix_name(dl::image::pix_type_t t)
{
    switch (t) {
    case dl::image::DL_IMAGE_PIX_TYPE_RGB565LE: return "RGB565LE";
    case dl::image::DL_IMAGE_PIX_TYPE_RGB565BE: return "RGB565BE";
    case dl::image::DL_IMAGE_PIX_TYPE_RGB888:   return "RGB888";
    case dl::image::DL_IMAGE_PIX_TYPE_GRAY:     return "GRAY";
    default:                                    return "OTHER";
    }
}

inline size_t b64_encode(const uint8_t *s, size_t n, char *o)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)s[i] << 16) | ((uint32_t)s[i + 1] << 8) | s[i + 2];
        o[j++] = T[(v >> 18) & 63];
        o[j++] = T[(v >> 12) & 63];
        o[j++] = T[(v >> 6) & 63];
        o[j++] = T[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)s[i] << 16;
        if (i + 1 < n) v |= (uint32_t)s[i + 1] << 8;
        o[j++] = T[(v >> 18) & 63];
        o[j++] = T[(v >> 12) & 63];
        o[j++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        o[j++] = '=';
    }
    return j;
}

inline void dump_frame(const dl::image::img_t &img, const std::list<dl::detect::result_t> &res)
{
    int mode = g_dump_req.exchange(DUMP_NONE);
    if (mode == DUMP_NONE || !img.data) {
        return;
    }
    const uint8_t *src = (const uint8_t *)img.data;
    const size_t px = img.col_step();
    const int step = (mode == DUMP_HALF) ? 2 : (mode == DUMP_QUARTER) ? 4 : 1;
    const int ow = (img.width + step - 1) / step;
    const int oh = (img.height + step - 1) / step;
    const size_t nbytes = (size_t)ow * oh * px;

    // Snapshot first: the camera ring buffer keeps running while we stream for seconds at 115200 baud.
#ifdef ESP_PLATFORM
    uint8_t *snap = (uint8_t *)heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint8_t *snap = (uint8_t *)std::malloc(nbytes);
#endif
    if (!snap) {
        std::printf("FRAME_ERROR,alloc %u\n", (unsigned)nbytes);
        return;
    }
    size_t o = 0;
    for (int y = 0; y < img.height; y += step) {
        const uint8_t *row = src + (size_t)y * img.row_step();
        for (int x = 0; x < img.width; x += step) {
            std::memcpy(snap + o, row + (size_t)x * px, px);
            o += px;
        }
    }

    std::printf("FRAME_BEGIN,%d,%d,%s,%u,%d,%u\n", ow, oh, pix_name(img.pix_type), (unsigned)nbytes, step,
                (unsigned)img.pix_type);
    for (const auto &r : res) {
        if (r.box.size() < 4) continue;
        std::printf("BOX,%d,%d,%d,%d,%d", r.box[0], r.box[1], r.box[2], r.box[3], (int)(r.score * 1000.f));
        for (int k : r.keypoint) std::printf(",%d", k);
        std::printf("\n");
    }
    char line[2 + 96 + 2];
    for (size_t off = 0; off < nbytes; off += 72) {
        size_t n = (nbytes - off < 72) ? (nbytes - off) : 72;
        line[0] = 'D';
        line[1] = ',';
        size_t m = b64_encode(snap + off, n, line + 2);
        line[2 + m] = '\n';
        line[3 + m] = 0;
        std::fputs(line, stdout);
#ifdef ESP_PLATFORM
        // The console UART write is a busy-wait. Without a periodic yield this task starves IDLE1 and the task
        // watchdog (5 s) fires during long dumps (full-res at 115200 baud takes ~14 s).
        if (((off / 72) & 7) == 7) {
            vTaskDelay(1);
        }
#endif
    }
    std::printf("FRAME_END\n");
    std::fflush(stdout);
#ifdef ESP_PLATFORM
    heap_caps_free(snap);
#else
    std::free(snap);
#endif
}
} // namespace bench
} // namespace who
