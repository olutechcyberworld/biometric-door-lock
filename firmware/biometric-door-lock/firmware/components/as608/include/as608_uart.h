#pragma once
#include "as608.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Opens the UART selected in menuconfig (AS608_*) and fills dev with the ESP32 transport. */
int as608_uart_open(as608_t *dev);
#ifdef __cplusplus
}
#endif
