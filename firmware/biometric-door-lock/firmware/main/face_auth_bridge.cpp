#include "face_auth_bridge.hpp"

#include <cstdlib>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

using who::recognition::WhoRecognitionCore;

namespace {
SemaphoreHandle_t s_sem;
volatile bool s_waiting;
volatile size_t s_last_faces; // faces seen in the frame that the next recognition result belongs to
struct {
    bool matched;
    int id;
    float sim;
    size_t faces;
} s_result;
} // namespace

void face_auth_on_detect(size_t faces)
{
    s_last_faces = faces;
}

void face_auth_on_result(const std::string &result)
{
    if (!s_waiting) {
        return; // nobody (or nobody still within their timeout) is waiting on this
    }
    s_result.faces = s_last_faces;
    // who_recognition.cpp emits exactly one of these two shapes: "who?" or "id: {}, sim: {:.2f}"
    if (result.rfind("id: ", 0) == 0) {
        s_result.matched = true;
        s_result.id = atoi(result.c_str() + 4);
        auto pos = result.find("sim: ");
        s_result.sim = (pos != std::string::npos) ? (float)atof(result.c_str() + pos + 5) : 0.f;
    } else {
        s_result.matched = false;
    }
    xSemaphoreGive(s_sem);
}

bool face_identify_blocking(WhoRecognitionCore *task, uint32_t timeout_ms, int *out_id, float *out_sim)
{
    if (!s_sem) {
        s_sem = xSemaphoreCreateBinary();
    }
    const TickType_t start = xTaskGetTickCount();
    const TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t used = xTaskGetTickCount() - start;
        if (used >= budget) {
            return false; // nobody (recognisable) stepped into view in time
        }
        xSemaphoreTake(s_sem, 0); // drain a stale give from a previous, already-abandoned wait
        s_waiting = true;
        xEventGroupSetBits(task->get_event_group(), WhoRecognitionCore::RECOGNIZE);
        bool got = xSemaphoreTake(s_sem, budget - used) == pdTRUE;
        s_waiting = false; // a result arriving from here on is for nobody and is dropped by the check above
        if (!got) {
            return false; // no frame was processed in time (camera stalled)
        }
        if (s_result.matched) {
            if (out_id) {
                *out_id = s_result.id;
            }
            if (out_sim) {
                *out_sim = s_result.sim;
            }
            return true;
        }
        if (s_result.faces > 0) {
            return false; // a face was seen and it is not enrolled: deny now, no further tries
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // empty frame: give the person a moment to step into view, then ask again
    }
}
