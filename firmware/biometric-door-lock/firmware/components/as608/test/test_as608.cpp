// Host-side unit tests for the AS608 driver, run against a simulated module. No hardware needed.
// Build/run: see tests/run_host_tests.sh
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>
extern "C" {
#include "as608.h"
}

static int g_fail = 0;
#define CHECK(c)                                                                                  \
    do {                                                                                          \
        if (!(c)) {                                                                               \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                            \
            g_fail++;                                                                             \
        }                                                                                         \
    } while (0)

// ------------------------------------------------------------------------------------------------
// Simulated AS608: speaks the real packet protocol, has a virtual clock, a scripted finger, 162 slots.
// ------------------------------------------------------------------------------------------------
struct FingerEvent { uint32_t on_ms, off_ms; int feature; };
struct Sim {
    std::deque<uint8_t> tx;              // bytes the module sends to the host
    std::vector<uint8_t> rxbuf;          // bytes from the host, being framed
    uint32_t now = 0;                    // virtual clock (ms)
    uint32_t cmd_latency_ms = 25;        // each command "takes" this long
    uint32_t address = 0xFFFFFFFF, password = 0;
    std::vector<FingerEvent> fingers;
    int buf[3] = {0, 0, 0};              // feature buffers 1,2 (index 1..2)
    std::map<int, int> library;          // id -> feature
    uint16_t capacity = 162;
    bool corrupt_next_checksum = false;
    bool mute = false;                   // module not connected

    int finger_now() const {
        for (auto &f : fingers) if (now >= f.on_ms && now < f.off_ms) return f.feature;
        return 0;
    }
    static uint16_t sum(const std::vector<uint8_t> &v, size_t a, size_t b) { uint32_t s = 0; for (size_t i = a; i < b; i++) s += v[i]; return (uint16_t)s; }
    void ack(const std::vector<uint8_t> &payload) {
        std::vector<uint8_t> f = {0xEF, 0x01, (uint8_t)(address >> 24), (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address, 0x07};
        uint16_t len = payload.size() + 2;
        f.push_back(len >> 8); f.push_back(len & 0xFF);
        f.insert(f.end(), payload.begin(), payload.end());
        uint16_t cs = sum(f, 6, f.size());
        if (corrupt_next_checksum) { cs ^= 0x0101; corrupt_next_checksum = false; }
        f.push_back(cs >> 8); f.push_back(cs & 0xFF);
        for (auto b : f) tx.push_back(b);
    }
    void handle(const std::vector<uint8_t> &pl) {
        now += cmd_latency_ms;
        uint8_t c = pl[0];
        switch (c) {
        case 0x13: { uint32_t p = (pl[1] << 24) | (pl[2] << 16) | (pl[3] << 8) | pl[4]; ack({(uint8_t)(p == password ? 0x00 : 0x13)}); break; }
        case 0x0F: ack({0x00, 0, 0, 0, 9, (uint8_t)(capacity >> 8), (uint8_t)capacity, 0, 3, 0xFF, 0xFF, 0xFF, 0xFF, 0, 2, 0, 6}); break;
        case 0x1D: ack({0x00, (uint8_t)(library.size() >> 8), (uint8_t)library.size()}); break;
        case 0x1F: { std::vector<uint8_t> r(33, 0); for (auto &kv : library) { int id = kv.first; if (id / 256 == pl[1]) r[1 + (id % 256) / 8] |= 1 << (id % 8); } ack(r); break; }
        case 0x01: ack({(uint8_t)(finger_now() ? 0x00 : 0x02)}); break;
        case 0x02: { int f = finger_now(); if (!f) { ack({0x02}); break; } buf[pl[1]] = f; ack({0x00}); now += 150; break; }
        case 0x05: ack({(uint8_t)(buf[1] && buf[1] == buf[2] ? 0x00 : 0x0A)}); break;
        case 0x06: { int id = (pl[2] << 8) | pl[3]; if (id >= capacity) { ack({0x0B}); break; } library[id] = buf[1]; ack({0x00}); break; }
        case 0x04: { now += 60; for (auto &kv : library) if (kv.second == buf[pl[1]]) { ack({0x00, (uint8_t)(kv.first >> 8), (uint8_t)kv.first, 0, 120}); return; } ack({0x09, 0, 0, 0, 0}); break; }
        case 0x0C: { int id = (pl[1] << 8) | pl[2], n = (pl[3] << 8) | pl[4]; for (int i = 0; i < n; i++) library.erase(id + i); ack({0x00}); break; }
        case 0x0D: library.clear(); ack({0x00}); break;
        case 0x0E: ack({0x00}); break;
        default: ack({0x0C});
        }
    }
    void feed(const uint8_t *d, size_t n) {
        if (mute) return;
        for (size_t i = 0; i < n; i++) rxbuf.push_back(d[i]);
        while (rxbuf.size() >= 9) {
            uint16_t len = (rxbuf[7] << 8) | rxbuf[8];
            if (rxbuf.size() < 9u + len) break;
            std::vector<uint8_t> pl(rxbuf.begin() + 9, rxbuf.begin() + 9 + len - 2);
            uint16_t cs = (rxbuf[9 + len - 2] << 8) | rxbuf[9 + len - 1];
            bool ok = rxbuf[0] == 0xEF && rxbuf[1] == 0x01 && cs == sum(rxbuf, 6, 9 + len - 2);
            rxbuf.erase(rxbuf.begin(), rxbuf.begin() + 9 + len);
            if (ok) handle(pl);
        }
    }
};
static Sim *S;
static std::vector<uint8_t> g_last_written;
static int t_write(const uint8_t *d, size_t n, void *) { g_last_written.assign(d, d + n); S->feed(d, n); return (int)n; }
static int t_read(uint8_t *d, size_t n, uint32_t timeout, void *) {
    size_t got = 0;
    while (got < n && !S->tx.empty()) { d[got++] = S->tx.front(); S->tx.pop_front(); }
    if (got == 0) S->now += timeout;   // nothing to read: the wait consumed the timeout
    return (int)got;
}
static void t_flush(void *) { S->tx.clear(); }
static uint32_t t_now(void *) { return S->now; }
static void t_delay(uint32_t ms, void *) { S->now += ms; }
static as608_t make(Sim &s) {
    S = &s; as608_t d; as608_transport_t io = {t_write, t_read, t_flush, t_now, t_delay, nullptr};
    as608_init(&d, io, 0xFFFFFFFF, 0); return d;
}

// ------------------------------------------------------------------------------------------------
static void test_known_frames() {
    std::printf("known packet vectors\n");
    Sim s; as608_t d = make(s);
    d.password = 0; as608_verify_password(&d);
    const uint8_t vfy[] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x07,0x13,0x00,0x00,0x00,0x00,0x00,0x1B};  // datasheet
    CHECK(g_last_written == std::vector<uint8_t>(vfy, vfy + sizeof vfy));
    as608_get_image(&d);
    const uint8_t gi[] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x03,0x01,0x00,0x05};
    CHECK(g_last_written == std::vector<uint8_t>(gi, gi + sizeof gi));
    as608_image_to_char(&d, 1);
    const uint8_t tz[] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x04,0x02,0x01,0x00,0x08};
    CHECK(g_last_written == std::vector<uint8_t>(tz, tz + sizeof tz));
    uint16_t id, sc; d.capacity = 163;
    as608_search(&d, 1, 0, 163, &id, &sc);
    const uint8_t se[] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,0x01,0x00,0x08,0x04,0x01,0x00,0x00,0x00,0xA3,0x00,0xB1};
    CHECK(g_last_written == std::vector<uint8_t>(se, se + sizeof se));
}

static void test_probe() {
    std::printf("probe / params / count\n");
    Sim s; as608_t d = make(s);
    CHECK(as608_verify_password(&d) == 0);
    as608_params_t p; CHECK(as608_read_params(&d, &p) == 0);
    CHECK(p.capacity == 162); CHECK(p.security_level == 3); CHECK(p.baud_multiplier == 6); CHECK(p.device_address == 0xFFFFFFFF);
    uint16_t n = 99; CHECK(as608_template_count(&d, &n) == 0); CHECK(n == 0);
    d.password = 0x1234; CHECK(as608_verify_password(&d) == 0x13);     // module says wrong password
}

static void test_comm_errors() {
    std::printf("communication failures\n");
    { Sim s; s.mute = true; as608_t d = make(s); CHECK(as608_verify_password(&d) == AS608_ERR_COMM); }
    { Sim s; as608_t d = make(s); s.corrupt_next_checksum = true; CHECK(as608_verify_password(&d) == AS608_ERR_CHECKSUM); CHECK(as608_verify_password(&d) == 0); }
    { Sim s; as608_t d = make(s); d.address = 0x00000001; CHECK(as608_verify_password(&d) == AS608_ERR_COMM); }  // wrong address: module ignores
}

static void test_enroll_identify() {
    std::printf("enroll then identify\n");
    Sim s; as608_t d = make(s);
    as608_params_t p; as608_read_params(&d, &p);
    s.fingers = {{200, 900, 77}, {1500, 2600, 77}};          // same finger placed twice
    std::vector<int> steps;
    auto cb = [](as608_enroll_step_t st, void *u) { ((std::vector<int> *)u)->push_back(st); };
    CHECK(as608_enroll(&d, 0, 10000, cb, &steps) == 0);
    CHECK((steps == std::vector<int>{AS608_STEP_PLACE1, AS608_STEP_REMOVE, AS608_STEP_PLACE2, AS608_STEP_STORING}));
    CHECK(s.library.count(0) == 1);
    s.fingers = {{s.now + 100, s.now + 5000, 77}};
    as608_identify_t r; int rc = as608_identify(&d, 8000, &r);
    CHECK(rc == 0); CHECK(r.id == 0); CHECK(r.score == 120); CHECK(r.wait_ms >= 100 && r.wait_ms < 400); CHECK(r.search_ms > 0);
    s.fingers = {{s.now + 100, s.now + 5000, 99}};               // a different finger
    rc = as608_identify(&d, 8000, &r); CHECK(rc == AS608_NOT_FOUND);
    s.fingers.clear();
    rc = as608_identify(&d, 1000, &r); CHECK(rc == AS608_ERR_TIMEOUT);     // nobody touches the sensor
}

static void test_enroll_mismatch() {
    std::printf("enroll with two different fingers\n");
    Sim s; as608_t d = make(s);
    s.fingers = {{100, 800, 11}, {1200, 2000, 22}};
    int rc = as608_enroll(&d, 3, 10000, nullptr, nullptr);
    CHECK(rc == AS608_MERGE_FAIL); CHECK(s.library.empty());
}

static void test_free_id_delete_empty() {
    std::printf("free id, delete, empty\n");
    Sim s; as608_t d = make(s); as608_params_t p; as608_read_params(&d, &p);
    s.library = {{0, 1}, {1, 2}, {3, 4}};
    uint16_t id = 99, used = 0; CHECK(as608_next_free_id(&d, &id, &used) == 0); CHECK(id == 2); CHECK(used == 3);
    CHECK(as608_delete(&d, 1, 1) == 0); CHECK(as608_next_free_id(&d, &id, &used) == 0); CHECK(id == 1); CHECK(used == 2);
    CHECK(as608_empty(&d) == 0); CHECK(s.library.empty());
    for (int i = 0; i < 162; i++) s.library[i] = i + 1;
    CHECK(as608_next_free_id(&d, &id, &used) == AS608_ERR_NO_SPACE); CHECK(used == 162);
}

int main() {
    test_known_frames(); test_probe(); test_comm_errors(); test_enroll_identify(); test_enroll_mismatch(); test_free_id_delete_empty();
    std::printf(g_fail ? "\n%d FAILED\n" : "\nALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
