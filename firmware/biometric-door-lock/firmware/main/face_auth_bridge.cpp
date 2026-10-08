#include "face_auth_bridge.hpp"
#include "esp_err.h"
#include "user_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

using who::recognition::WhoRecognitionCore;

namespace {
SemaphoreHandle_t s_sem;
SemaphoreHandle_t s_db_mu; // serialises door auth, enrollment and deletion against the recognition task
HumanFaceRecognizer *s_recognizer;
volatile bool s_waiting;
volatile bool s_enroll_mode; // the waiter wants an enroll result, not a recognise result
volatile bool s_enroll_ok;
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
    if (s_enroll_mode) {
        // who_recognition.cpp: "id: N enrolled." on success, "Failed to enroll." when the frame had no usable face
        s_enroll_ok = result.rfind("id: ", 0) == 0 && result.find("enrolled") != std::string::npos;
        xSemaphoreGive(s_sem);
        return;
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

void face_auth_init(HumanFaceRecognizer *recognizer)
{
    s_recognizer = recognizer;
    if (!s_sem) {
        s_sem = xSemaphoreCreateBinary();
    }
    if (!s_db_mu) {
        s_db_mu = xSemaphoreCreateMutex();
    }
}

namespace {
struct DbLock {
    DbLock()
    {
        if (s_db_mu) xSemaphoreTake(s_db_mu, portMAX_DELAY);
    }
    ~DbLock()
    {
        if (s_db_mu) xSemaphoreGive(s_db_mu);
    }
};

// esp-dl keeps {num_feats_total, num_feats_valid, feat_len} (3 x uint16) at the start of the file and hands out ids as
// num_feats_total + 1, never reusing one: after an enroll the new id is simply num_feats_total.
int face_db_total()
{
    char path[64];
    snprintf(path, sizeof(path), "%s/face.db", CONFIG_SPIFLASH_MOUNT_POINT);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint16_t meta[3] = {0, 0, 0};
    size_t n = fread(meta, sizeof(uint16_t), 3, f);
    fclose(f);
    return n == 3 ? (int)meta[0] : -1;
}
} // namespace

bool face_enroll_blocking(WhoRecognitionCore *task, uint32_t timeout_ms, uint16_t *out_id)
{
    if (!s_sem || !task) return false;
    DbLock lock;
    xSemaphoreTake(s_sem, 0);
    s_enroll_ok = false;
    s_enroll_mode = true;
    s_waiting = true;
    xEventGroupSetBits(task->get_event_group(), WhoRecognitionCore::ENROLL);
    bool got = xSemaphoreTake(s_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    s_waiting = false;
    s_enroll_mode = false;
    if (!got || !s_enroll_ok) return false;
    int total = face_db_total();
    if (total <= 0) return false;
    if (out_id) *out_id = (uint16_t)total;
    return true;
}

int face_stored_id_for_position(int position)
{
    DbLock lock;
    char path[64];
    snprintf(path, sizeof(path), "%s/face.db", CONFIG_SPIFLASH_MOUNT_POINT);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int id = facedb_stored_id_at(f, position);
    fclose(f);
    return id;
}

int face_prune(bool (*keep)(uint16_t id), int *kept)
{
    if (!s_recognizer) return -1;
    DbLock lock;
    const int before = s_recognizer->get_num_feats();
    if (!keep) {
        if (s_recognizer->clear_all_feats() != ESP_OK) return -1;
        if (kept) *kept = 0;
        return before;
    }
    char path[64];
    snprintf(path, sizeof(path), "%s/face.db", CONFIG_SPIFLASH_MOUNT_POINT);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint16_t doomed[256];
    int n = 0;
    for (int pos = 1; n < 256; pos++) {
        int id = facedb_stored_id_at(f, pos);
        if (id < 0) break;
        if (!keep((uint16_t)id)) doomed[n++] = (uint16_t)id;
    }
    fclose(f);
    int deleted = 0;
    for (int i = 0; i < n; i++) {
        if (s_recognizer->delete_feat(doomed[i]) == ESP_OK) deleted++;
    }
    if (kept) *kept = before - deleted;
    return deleted;
}

bool face_delete(uint16_t id)
{
    if (!s_recognizer) return false;
    DbLock lock;
    return s_recognizer->delete_feat(id) == ESP_OK;
}

bool face_identify_blocking(WhoRecognitionCore *task, uint32_t timeout_ms, int *out_id, float *out_sim)
{
    if (!s_sem) {
        s_sem = xSemaphoreCreateBinary();
    }
    DbLock lock; // the whole attempt, so an enrollment/deletion cannot interleave with the recognition task
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
