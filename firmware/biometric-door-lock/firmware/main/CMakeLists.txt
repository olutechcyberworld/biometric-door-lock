set(src_dirs        ./)

set(include_dirs    ./)

set(requires who_spiflash_fatfs
             who_recognition_app
             who_recognition
             who_detect
             esp_video_cxx
             esp_timer
             as608
             auth_fsm
             relay_control
             retry_lockout
             wifi_manager
             device_link
             pin_auth
             user_store
             jpeg_lite
             json
             esp_http_server
             nvs_flash
             esp_hw_support)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
