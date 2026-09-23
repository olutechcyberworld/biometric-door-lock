#pragma once
// Counts frames actually delivered by the camera driver to the pipeline (before any detection).
// Lets the dashboard tell "camera delivers nothing" apart from "frames arrive but the detector isn't running".
#include <atomic>
#include <cstdint>
namespace who {
namespace bench {
inline std::atomic<uint32_t> g_cam_frames{0};
} // namespace bench
} // namespace who
