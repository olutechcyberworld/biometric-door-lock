#pragma once
/*
 * AS608 / R30x-family optical fingerprint module driver (UART protocol).
 *
 * Platform independent: all I/O goes through as608_transport_t so the same code runs on the ESP32-S3 (as608_uart.c) and in
 * the host unit tests (test/), which run it against a simulated module.
 *
 * Return values: 0 = OK, >0 = the module's own confirmation code (see as608_strerror), <0 = local error.
 * Biometric templates stay inside the module; nothing here reads or writes template data.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AS608_OK 0x00
#define AS608_ERR_COMM (-1)     /* no / malformed response: wiring, baud, power, or wrong address            */
#define AS608_ERR_CHECKSUM (-2) /* response arrived but its checksum is wrong (noise, wrong baud)            */
#define AS608_ERR_TIMEOUT (-3)  /* waiting for a finger timed out                                            */
#define AS608_ERR_ARG (-4)
#define AS608_ERR_NO_SPACE (-5) /* library full                                                              */

/* Module confirmation codes used by this driver */
#define AS608_NO_FINGER 0x02
#define AS608_IMAGE_FAIL 0x03
#define AS608_IMAGE_MESSY 0x06
#define AS608_FEW_FEATURES 0x07
#define AS608_NO_MATCH 0x08
#define AS608_NOT_FOUND 0x09
#define AS608_MERGE_FAIL 0x0A
#define AS608_BAD_PAGE 0x0B

typedef struct {
    int (*write)(const uint8_t *data, size_t len, void *ctx);               /* bytes written, or <0            */
    int (*read)(uint8_t *data, size_t len, uint32_t timeout_ms, void *ctx); /* bytes read (0 on timeout), or <0 */
    void (*flush_rx)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    void (*delay_ms)(uint32_t ms, void *ctx);
    void *ctx;
} as608_transport_t;

typedef struct {
    as608_transport_t io;
    uint32_t address;  /* module address, default 0xFFFFFFFF */
    uint32_t password; /* default 0x00000000                 */
    uint16_t capacity; /* filled by as608_read_params        */
    /* Diagnostics of the LAST exchange, so a failed probe says WHY it failed:
     * dbg_stage: 0 = silence (0 bytes), 1 = bytes arrived but no EF01 header, 2 = header seen but address/PID/length
     *            rejected, 3 = packet cut short, 4 = checksum mismatch, 5 = ok                                   */
    uint16_t dbg_rx;
    uint8_t dbg_stage;
} as608_t;

typedef struct {
    uint16_t status_register;
    uint16_t system_id;
    uint16_t capacity;
    uint16_t security_level; /* 1 (lowest FAR strictness) .. 5 */
    uint32_t device_address;
    uint16_t packet_size_code;
    uint16_t baud_multiplier; /* baud = 9600 * N */
} as608_params_t;

typedef struct {
    uint16_t id;
    uint16_t score;
    uint32_t wait_ms;    /* until a finger was seen                       */
    uint32_t capture_ms; /* image -> feature extraction                   */
    uint32_t search_ms;  /* on-module 1:N search                          */
} as608_identify_t;

typedef enum { AS608_STEP_PLACE1, AS608_STEP_REMOVE, AS608_STEP_PLACE2, AS608_STEP_STORING } as608_enroll_step_t;
typedef void (*as608_enroll_cb)(as608_enroll_step_t step, void *user);

void as608_init(as608_t *dev, as608_transport_t io, uint32_t address, uint32_t password);
const char *as608_strerror(int code);

/* Basic commands (one request/response each) */
int as608_verify_password(as608_t *dev);
int as608_read_params(as608_t *dev, as608_params_t *out);
int as608_template_count(as608_t *dev, uint16_t *count);
int as608_set_security_level(as608_t *dev, uint8_t level);
int as608_get_image(as608_t *dev);                     /* 0 = image captured, AS608_NO_FINGER = nothing on glass */
int as608_image_to_char(as608_t *dev, uint8_t buffer); /* buffer 1 or 2                                          */
int as608_reg_model(as608_t *dev);                     /* merge buffers 1+2 into a template                      */
int as608_store(as608_t *dev, uint8_t buffer, uint16_t id);
int as608_search(as608_t *dev, uint8_t buffer, uint16_t start, uint16_t count, uint16_t *id, uint16_t *score);
int as608_delete(as608_t *dev, uint16_t id, uint16_t count);
int as608_empty(as608_t *dev);
int as608_read_index_table(as608_t *dev, uint8_t page, uint8_t out[32]); /* bitmap of used ids, page*256.. */

/* Blocking workflows */
int as608_capture(as608_t *dev, uint8_t buffer, uint32_t timeout_ms, uint32_t *wait_ms, uint32_t *capture_ms);
int as608_identify(as608_t *dev, uint32_t timeout_ms, as608_identify_t *out);
int as608_enroll(as608_t *dev, uint16_t id, uint32_t timeout_ms, as608_enroll_cb cb, void *user);
int as608_next_free_id(as608_t *dev, uint16_t *id, uint16_t *used_count);

#ifdef __cplusplus
}
#endif
