#include "user_db.hpp"
#include "esp_err.h"
#include "esp_timer.h"
#include "face_auth_bridge.hpp"
#include "fingerprint_task.hpp"
#include "session_ws.hpp"

#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char *NVS_NS = "users";
constexpr const char *NVS_KEY = "tab";
user_store_t s_store;
SemaphoreHandle_t s_mu;
bool s_warned_legacy;

struct Lock {
    Lock() { xSemaphoreTake(s_mu, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mu); }
};

bool persist_locked()
{
    uint8_t blob[USER_BLOB_LEN];
    user_store_serialize(&s_store, blob);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, NVS_KEY, blob, sizeof(blob)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
} // namespace

void user_db_start()
{
    s_mu = xSemaphoreCreateMutex();
    esp_err_t e = nvs_flash_init(); // idempotent: wifi_manager_start() does the same later
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    user_store_init(&s_store);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t blob[USER_BLOB_LEN];
        size_t len = sizeof(blob);
        if (nvs_get_blob(h, NVS_KEY, blob, &len) == ESP_OK) {
            user_store_t loaded;
            if (user_store_deserialize(&loaded, blob, len)) {
                s_store = loaded;
            } else {
                printf("USERS,load=corrupt,action=ignored\n");
            }
        }
        nvs_close(h);
    }
    printf("USERS,count=%d\n", user_store_count(&s_store));
}

bool user_db_authorize(uint16_t fp_slot, uint16_t face_id)
{
    Lock l;
    if (user_store_count(&s_store) == 0) {
        if (!s_warned_legacy) {
            s_warned_legacy = true;
            printf("USERS,warn=no users recorded,binding=off (any enrolled face passes); enroll a user from the app\n");
        }
        return true;
    }
    return user_store_face_matches_finger(&s_store, fp_slot, face_id);
}

int user_db_count()
{
    Lock l;
    return user_store_count(&s_store);
}

bool user_db_has_fp_slot(uint16_t fp_slot)
{
    Lock l;
    return user_store_has_fp_slot(&s_store, fp_slot);
}

uint16_t user_db_add(const char *name, uint16_t fp_slot, const uint16_t *face_ids, uint8_t n_faces)
{
    Lock l;
    user_store_t backup = s_store;
    uint16_t id = user_store_add(&s_store, name, fp_slot, face_ids, n_faces);
    if (id && !persist_locked()) {
        s_store = backup; // not stored = not enrolled
        return 0;
    }
    return id;
}

bool user_db_remove(uint16_t id, user_rec_t *removed)
{
    Lock l;
    user_store_t backup = s_store;
    if (!user_store_remove(&s_store, id, removed)) return false;
    if (!persist_locked()) {
        s_store = backup;
        return false;
    }
    return true;
}

bool user_db_list_json(char *out, size_t cap)
{
    Lock l;
    size_t p = (size_t)snprintf(out, cap, "{\"type\":\"user_list\",\"users\":[");
    bool first = true;
    for (int i = 0; i < USER_MAX; i++) {
        const user_rec_t &u = s_store.u[i];
        if (!u.id) continue;
        if (p >= cap) return false;
        p += (size_t)snprintf(out + p, cap - p, "%s{\"id\":\"%u\",\"name\":\"%s\"}", first ? "" : ",", (unsigned)u.id, u.name);
        first = false;
    }
    if (p >= cap) return false;
    p += (size_t)snprintf(out + p, cap - p, "]}");
    return p < cap;
}

bool user_db_get(uint16_t id, user_rec_t *out)
{
    Lock l;
    return user_store_get(&s_store, id, out);
}

void user_db_console_prune()
{
    static int64_t armed_us = 0;
    if (user_db_count() == 0) {
        printf("USERS,prune=refused,reason=no users yet (it would delete every fingerprint)\n");
        return;
    }
    int64_t now = esp_timer_get_time();
    if (armed_us && now - armed_us < 5000000) {
        armed_us = 0;
        if (!fingerprint_prune(user_db_has_fp_slot)) printf("USERS,prune=refused,reason=sensor busy\n");
        return;
    }
    armed_us = now;
    printf("USERS,prune=armed,send U again within 5 s to delete every fingerprint that belongs to no user\n");
}

bool user_db_has_face_id(uint16_t face_id)
{
    Lock l;
    return user_store_has_face(&s_store, face_id);
}

void user_db_console_prune_faces()
{
    static int64_t armed_us = 0;
    if (session_ws_active()) {
        printf("FACES,prune=refused,reason=owner session active (faces being enrolled are not in the table yet)\n");
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!(armed_us && now - armed_us < 5000000)) {
        armed_us = now;
        printf("FACES,prune=armed,send F again within 5 s to %s\n",
               user_db_count() == 0 ? "WIPE the whole face database" : "delete every face that belongs to no user");
        return;
    }
    armed_us = 0;
    int kept = 0;
    int deleted = face_prune(user_db_count() == 0 ? nullptr : user_db_has_face_id, &kept);
    if (deleted < 0) printf("FACES,prune=failed\n");
    else printf("FACES,prune,deleted=%d,kept=%d\n", deleted, kept);
}
