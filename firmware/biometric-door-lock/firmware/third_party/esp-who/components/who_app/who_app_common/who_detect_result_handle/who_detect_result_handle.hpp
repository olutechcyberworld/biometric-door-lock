#pragma once
#include "who_detect.hpp"
#include <array>
#include <queue>
#include <string>
#include <utility>
#include <vector>
#include "bsp/esp-bsp.h"

namespace who {
namespace detect {
void draw_detect_results_on_img(const dl::image::img_t &img,
                                const std::list<dl::detect::result_t> &detect_res,
                                const std::vector<std::vector<uint8_t>> &palette);

#if !BSP_CONFIG_NO_GRAPHIC_LIB
void draw_detect_results_on_canvas(lv_obj_t *canvas,
                                   const std::list<dl::detect::result_t> &detect_res,
                                   const std::vector<lv_color_t> &palette);
#endif

void print_detect_results(const std::list<dl::detect::result_t> &detect_res);
} // namespace detect

namespace lcd_disp {
// A single element to be drawn on the display: a box with its own color,
// an optional text label at the top-left corner of the box, optional
// keypoints and an optional cross at the box center.
struct draw_item_t {
    std::array<int, 4> box;     // (x1, y1, x2, y2)
    std::vector<uint8_t> color; // RGB888
    std::string label;          // text at the top-left corner of the box, empty for none
    std::vector<int> keypoint;  // (x, y) pairs, empty for none
    bool cross = false;         // draw a cross at the center of the box
};

class WhoDetectResultLCDDisp {
public:
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    WhoDetectResultLCDDisp(task::WhoTask *task, lv_obj_t *canvas, const std::vector<std::vector<uint8_t>> &palette);
#else
    WhoDetectResultLCDDisp(task::WhoTask *task, const std::vector<std::vector<uint8_t>> &palette);
#endif
    ~WhoDetectResultLCDDisp();
    // Convert a detect result into draw items (color by category) and save them.
    void save_detect_result(const detect::WhoDetect::result_t &result);
    // Save explicitly provided draw items (box, color, label, ...).
    void save_draw_items(const std::vector<draw_item_t> &items, int64_t timestamp);
    void lcd_disp_cb(VideoCapture::Frame *fb);
    void cleanup();

private:
    task::WhoTask *m_task;
    SemaphoreHandle_t m_res_mutex;
    std::queue<std::pair<int64_t, std::vector<draw_item_t>>> m_results;
    std::pair<int64_t, std::vector<draw_item_t>> m_result;
    std::vector<std::vector<uint8_t>> m_palette; // RGB888, for save_detect_result
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    lv_obj_t *m_canvas;
#endif
};
} // namespace lcd_disp
} // namespace who
