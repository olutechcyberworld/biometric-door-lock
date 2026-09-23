idf_component_register(SRCS "as608.c" "as608_uart.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_timer)
