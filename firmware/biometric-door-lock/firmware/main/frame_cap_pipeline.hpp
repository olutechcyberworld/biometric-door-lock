#pragma once
#include "who_frame_cap.hpp"

#if CONFIG_IDF_TARGET_ESP32S3
who::frame_cap::WhoFrameCap *get_dvp_frame_cap_pipeline();
#elif CONFIG_IDF_TARGET_ESP32P4
who::frame_cap::WhoFrameCap *get_mipi_csi_frame_cap_pipeline();
#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
who::frame_cap::WhoFrameCap *get_uvc_frame_cap_pipeline();
#endif
#endif
