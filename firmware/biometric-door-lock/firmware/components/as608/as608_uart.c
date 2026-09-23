#include "as608_uart.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define AS608_PORT ((uart_port_t)CONFIG_AS608_UART_NUM)

static int t_write(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    return uart_write_bytes(AS608_PORT, data, len);
}

static int t_read(uint8_t *data, size_t len, uint32_t timeout_ms, void *ctx)
{
    (void)ctx;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (ticks == 0) {
        ticks = 1;
    }
    return uart_read_bytes(AS608_PORT, data, len, ticks);
}

static void t_flush(void *ctx)
{
    (void)ctx;
    uart_flush_input(AS608_PORT);
}

static uint32_t t_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void t_delay(uint32_t ms, void *ctx)
{
    (void)ctx;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

int as608_uart_open(as608_t *dev)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_AS608_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(AS608_PORT, 512, 512, 0, NULL, 0) != ESP_OK) {
        return AS608_ERR_COMM;
    }
    if (uart_param_config(AS608_PORT, &cfg) != ESP_OK ||
        uart_set_pin(AS608_PORT, CONFIG_AS608_TX_GPIO, CONFIG_AS608_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) !=
            ESP_OK) {
        return AS608_ERR_COMM;
    }
    as608_transport_t io = {
        .write = t_write,
        .read = t_read,
        .flush_rx = t_flush,
        .now_ms = t_now,
        .delay_ms = t_delay,
        .ctx = NULL,
    };
    as608_init(dev, io, 0xFFFFFFFFu, 0x00000000u);
    return AS608_OK;
}
