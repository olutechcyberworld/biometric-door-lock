idf_component_register(SRCS "wifi_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_event esp_timer esp_hw_support esp_http_server
                                nvs_flash lwip mdns relay_control)
