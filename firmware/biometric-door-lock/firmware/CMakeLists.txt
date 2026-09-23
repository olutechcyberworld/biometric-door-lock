# Door-lock firmware: standalone ESP-IDF project (ESP32-S3, ESP-IDF 5.5.x).
#   idf.py set-target esp32s3 && idf.py build      (see ../scripts/ and ../README.md)
# ESP-WHO components are vendored in third_party/esp-who (see VENDORED.md there), so no environment tricks
# (IDF_EXTRA_ACTIONS_PATH, BSP flags) are needed: the board support package is declared in main/idf_component.yml.
cmake_minimum_required(VERSION 3.16)

set(ESP_WHO ${CMAKE_CURRENT_LIST_DIR}/third_party/esp-who/components)
set(EXTRA_COMPONENT_DIRS
    ${ESP_WHO}/who_task
    ${ESP_WHO}/who_peripherals/esp_video_cxx
    ${ESP_WHO}/who_peripherals/who_lcd
    ${ESP_WHO}/who_peripherals/who_spiflash_fatfs
    ${ESP_WHO}/who_frame_cap
    ${ESP_WHO}/who_frame_lcd_disp
    ${ESP_WHO}/who_detect
    ${ESP_WHO}/who_recognition
    ${ESP_WHO}/who_app/who_recognition_app)

add_compile_options(-fdiagnostics-color=always)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(door_lock)
