// Phase 1 bench build: headless (no LCD, no physical buttons), serial-triggered.
//   r = recognize (one attempt)   e = enroll (current face)   d = delete last enrolled
//   f/n/p/X = fingerprint (see fingerprint_task.hpp)
//   v/h/q = dump next frame as seen by the detector: full 240x240 / half 120x120 / quarter 60x60
//   HB,uptime_ms,frames,detect_us  is printed every 2 s (heartbeat for the dashboard)
// Every recognize attempt prints one line:
//   BENCH,seq,faces,detect_us,recog_us,e2e_us,id,sim
#include <cstdio>
#include "esp_log.h"
#include "sdkconfig.h"
#include "frame_cap_pipeline.hpp"
#include "human_face_detect.hpp"
#include "who_bench.hpp"
#include "who_recognition_app_base.hpp"
#include "who_spiflash_fatfs.hpp"
#include "who_yield2idle.hpp"
#include "fingerprint_task.hpp"

using who::recognition::WhoRecognitionCore;

static const char *TAG = "bench_app";

namespace who {
namespace app {
// Same wiring as WhoRecognitionAppTerm, minus the physical-button hookup
// (the S3-EYE button pins are not connected on this board and would float).
// Kept inside who::app so name lookup matches the stock Term app exactly.
class BenchApp : public WhoRecognitionAppBase {
public:
    BenchApp(frame_cap::WhoFrameCap *frame_cap) : WhoRecognitionAppBase(frame_cap)
    {
        auto recog = m_recognition->get_recognition_task();
        recog->set_recognition_result_cb([](const std::string &r) { ESP_LOGI(TAG, "%s", r.c_str()); });
        char db_path[64];
        snprintf(db_path, sizeof(db_path), "%s/face.db", CONFIG_SPIFLASH_MOUNT_POINT);
        m_recognition->set_recognizer(new HumanFaceRecognizer(db_path));
        m_recognition->set_detect_model(new HumanFaceDetect());
    }

    bool run() override
    {
        bool ret = WhoYield2Idle::get_instance()->run();
        for (const auto &node : m_frame_cap->get_all_nodes()) {
            ret &= node->run(4096, 2, 0);
        }
        ret &= m_recognition->get_detect_task()->run(6144, 2, 1); // printf/dump run in this task
        ret &= m_recognition->get_recognition_task()->run(3584, 2, 1);
        return ret;
    }

    recognition::WhoRecognitionCore *recog_task()
    {
        return m_recognition->get_recognition_task();
    }
};
} // namespace app
} // namespace who

static void console_task(void *arg)
{
    auto *task = static_cast<WhoRecognitionCore *>(arg);
#ifdef CONFIG_DOORLOCK_FINGERPRINT
    printf("FWINFO,viewer=2,modes=qhv,fp=1\n");
#else
    printf("FWINFO,viewer=2,modes=qhv,fp=0\n");
#endif
    printf("console ready: r=recognize e=enroll d=delete-last v/h/q=frame f=finger-identify n=finger-enroll p=probe X=finger-erase\n");
    int64_t last_hb_us = 0;
    while (true) {
        int c = getchar();
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_hb_us >= 2000000) {
            last_hb_us = now_us;
            who::bench::heartbeat();
        }
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        EventBits_t bit = 0;
        switch (c) {
        case 'r':
            bit = WhoRecognitionCore::RECOGNIZE;
            who::bench::arm();
            break;
        case 'e':
            bit = WhoRecognitionCore::ENROLL;
            break;
        case 'd':
            bit = WhoRecognitionCore::DELETE;
            break;
        case 'v':
            who::bench::g_dump_req.store(who::bench::DUMP_FULL);
            continue;
        case 'h':
            who::bench::g_dump_req.store(who::bench::DUMP_HALF);
            continue;
        case 'q':
            who::bench::g_dump_req.store(who::bench::DUMP_QUARTER);
            continue;
        case 'f': // fingerprint: identify / enroll next / probe / erase all (see fingerprint_task.hpp)
        case 'n':
        case 'p':
        case 'X':
            fingerprint_post((char)c);
            continue;
        default:
            continue;
        }
        if (task->is_active()) {
            xEventGroupSetBits(task->get_event_group(), bit);
        }
    }
}

extern "C" void app_main(void)
{
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);
    ESP_ERROR_CHECK(spiflash_fatfs_mount());
    fingerprint_start(); // independent of the camera: own UART, own task on core 0

    auto frame_cap = get_dvp_frame_cap_pipeline();
    auto app = new who::app::BenchApp(frame_cap);
    if (!app->run()) {
        ESP_LOGE(TAG, "app->run() failed");
        return;
    }
    xTaskCreate(console_task, "console", 4096, app->recog_task(), 3, nullptr);
}
