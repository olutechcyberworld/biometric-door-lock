idf_component_register(SRCS "device_link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_netif esp_hw_support lwip mdns wifi_manager)
