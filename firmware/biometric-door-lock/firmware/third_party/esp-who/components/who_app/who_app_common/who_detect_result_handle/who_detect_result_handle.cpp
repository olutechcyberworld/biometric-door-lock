#include "who_detect_result_handle.hpp"
#if !BSP_CONFIG_NO_GRAPHIC_LIB
#include "who_lvgl_utils.hpp"
#endif
#include "dl_image_pixel_cvt_dispatch.hpp"
#include <algorithm>

namespace who {
namespace detect {
void draw_detect_results_on_img(const dl::image::img_t &img,
                                const std::list<dl::detect::result_t> &detect_res,
                                const std::vector<std::vector<uint8_t>> &palette)
{
    for (const auto &res : detect_res) {
        dl::image::draw_hollow_rectangle(img, res.box[0], res.box[1], res.box[2], res.box[3], palette[res.category], 2);
        if (!res.keypoint.empty()) {
            assert(res.keypoint.size() == 10);
            for (int i = 0; i < 5; i++) {
                dl::image::draw_point(img, res.keypoint[2 * i], res.keypoint[2 * i + 1], palette[res.category], 3);
            }
        }
    }
}

#if !BSP_CONFIG_NO_GRAPHIC_LIB
void draw_detect_results_on_canvas(lv_obj_t *canvas,
                                   const std::list<dl::detect::result_t> &detect_res,
                                   const std::vector<lv_color_t> &palette)
{
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_TRANSP;
    rect_dsc.border_width = 2;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.width = 5;
    arc_dsc.radius = 5;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_area_t coords_rect;
    for (const auto &res : detect_res) {
        coords_rect = {res.box[0], res.box[1], res.box[2], res.box[3]};
        rect_dsc.border_color = palette[res.category];
        lv_draw_rect(&layer, &rect_dsc, &coords_rect);
        if (!res.keypoint.empty()) {
            arc_dsc.color = palette[res.category];
            assert(res.keypoint.size() == 10);
            for (int i = 0; i < 5; i++) {
                arc_dsc.center.x = res.keypoint[2 * i];
                arc_dsc.center.y = res.keypoint[2 * i + 1];
                lv_draw_arc(&layer, &arc_dsc);
            }
        }
    }
    lv_canvas_finish_layer(canvas, &layer);
}
#endif

void print_detect_results(const std::list<dl::detect::result_t> &detect_res)
{
    int i = 0;
    const char *TAG = "detect";
    if (!detect_res.empty()) {
        if (detect_res.begin()->keypoint.empty()) {
            ESP_LOGI(TAG, "----------------------------------------");
        } else {
            ESP_LOGI(
                TAG,
                "---------------------------------------------------------------------------------------------------"
                "---------------------------------------------------");
        }
    }
    for (const auto &r : detect_res) {
        if (r.keypoint.empty()) {
            ESP_LOGI(TAG, "%d, bbox: [%f, %d, %d, %d, %d]", i, r.score, r.box[0], r.box[1], r.box[2], r.box[3]);
        } else {
            assert(r.keypoint.size() == 10);
            ESP_LOGI(TAG,
                     "%d, bbox: [%f, %d, %d, %d, %d], left_eye: [%d, %d], left_mouth: [%d, %d], nose: [%d, %d], "
                     "right_eye: [%d, %d], right_mouth: [%d, %d]",
                     i,
                     r.score,
                     r.box[0],
                     r.box[1],
                     r.box[2],
                     r.box[3],
                     r.keypoint[0],
                     r.keypoint[1],
                     r.keypoint[2],
                     r.keypoint[3],
                     r.keypoint[4],
                     r.keypoint[5],
                     r.keypoint[6],
                     r.keypoint[7],
                     r.keypoint[8],
                     r.keypoint[9]);
        }
        i++;
    }
}
} // namespace detect

namespace lcd_disp {
#if !BSP_CONFIG_NO_GRAPHIC_LIB
static void draw_items_on_canvas(lv_obj_t *canvas, const std::vector<draw_item_t> &items)
{
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_TRANSP;
    rect_dsc.border_width = 2;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.width = 5;
    arc_dsc.radius = 5;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.width = 2;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_area_t coords_rect;
    for (const auto &item : items) {
        lv_color_t color = cvt_to_lv_color(item.color);
        coords_rect = {item.box[0], item.box[1], item.box[2], item.box[3]};
        rect_dsc.border_color = color;
        lv_draw_rect(&layer, &rect_dsc, &coords_rect);
        if (!item.label.empty()) {
            label_dsc.color = color;
            label_dsc.text = item.label.c_str();
            const lv_font_t *font = label_dsc.font ? label_dsc.font : LV_FONT_DEFAULT;
            lv_area_t coords_label = {item.box[0], item.box[1], item.box[2], item.box[1] + font->line_height};
            lv_draw_label(&layer, &label_dsc, &coords_label);
        }
        if (item.cross) {
            int cx = (item.box[0] + item.box[2]) / 2;
            int cy = (item.box[1] + item.box[3]) / 2;
            static constexpr int kCrossHalfLen = 10;
            line_dsc.color = color;
            line_dsc.p1 = {cx - kCrossHalfLen, cy};
            line_dsc.p2 = {cx + kCrossHalfLen, cy};
            lv_draw_line(&layer, &line_dsc);
            line_dsc.p1 = {cx, cy - kCrossHalfLen};
            line_dsc.p2 = {cx, cy + kCrossHalfLen};
            lv_draw_line(&layer, &line_dsc);
        }
        if (!item.keypoint.empty()) {
            arc_dsc.color = color;
            for (size_t i = 0; i + 1 < item.keypoint.size(); i += 2) {
                arc_dsc.center.x = item.keypoint[i];
                arc_dsc.center.y = item.keypoint[i + 1];
                lv_draw_arc(&layer, &arc_dsc);
            }
        }
    }
    lv_canvas_finish_layer(canvas, &layer);
}
#endif

#if !BSP_CONFIG_NO_GRAPHIC_LIB
WhoDetectResultLCDDisp::WhoDetectResultLCDDisp(task::WhoTask *task,
                                               lv_obj_t *canvas,
                                               const std::vector<std::vector<uint8_t>> &palette) :
    m_task(task), m_res_mutex(xSemaphoreCreateMutex()), m_result(), m_palette(palette), m_canvas(canvas)
{
}
#else
WhoDetectResultLCDDisp::WhoDetectResultLCDDisp(task::WhoTask *task, const std::vector<std::vector<uint8_t>> &palette) :
    m_task(task), m_res_mutex(xSemaphoreCreateMutex()), m_result(), m_palette(palette)
{
}
#endif

WhoDetectResultLCDDisp::~WhoDetectResultLCDDisp()
{
    vSemaphoreDelete(m_res_mutex);
}

void WhoDetectResultLCDDisp::save_detect_result(const detect::WhoDetect::result_t &result)
{
    std::vector<draw_item_t> items;
    items.reserve(result.det_res.size());
    for (const auto &res : result.det_res) {
        draw_item_t item;
        item.box = {res.box[0], res.box[1], res.box[2], res.box[3]};
        item.color = m_palette[res.category];
        item.keypoint = res.keypoint;
        items.push_back(std::move(item));
    }
    save_draw_items(items, result.timestamp);
}

void WhoDetectResultLCDDisp::save_draw_items(const std::vector<draw_item_t> &items, int64_t timestamp)
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    m_results.push({timestamp, items});
    xSemaphoreGive(m_res_mutex);
}

void WhoDetectResultLCDDisp::lcd_disp_cb(VideoCapture::Frame *fb)
{
    if (!m_task->is_active()) {
        return;
    }
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    // // Try to sync camera frame and result, skip the future result.
    // auto compare_timestamp = [](const struct timeval &t1, const struct timeval &t2) -> bool {
    //     if (t1.tv_sec == t2.tv_sec) {
    //         return t1.tv_usec < t2.tv_usec;
    //     }
    //     return t1.tv_sec < t2.tv_sec;
    // };
    int64_t t1 = fb->timestamp;
    // If detect fps higher than display fps, the result queue may be more than 1. May happen when using lvgl.
    while (!m_results.empty()) {
        std::pair<int64_t, std::vector<draw_item_t>> result = m_results.front();
        if (t1 >= result.first) {
            m_result = std::move(result);
            m_results.pop();
        } else {
            break;
        }
    }
    xSemaphoreGive(m_res_mutex);
#if BSP_CONFIG_NO_GRAPHIC_LIB
    if (fb->pixel_format == V4L2_PIX_FMT_RGB565 || fb->pixel_format == V4L2_PIX_FMT_RGB565X ||
        fb->pixel_format == V4L2_PIX_FMT_RGB24) {
        bool is_rgb565 = (fb->pixel_format != V4L2_PIX_FMT_RGB24);
        auto img = frame2img(fb);
        for (const auto &item : m_result.second) {
            // Track boxes can extend past the frame (Kalman prediction), clip them.
            int x1 = std::max(item.box[0], 0);
            int y1 = std::max(item.box[1], 0);
            int x2 = std::min(item.box[2], (int)img.width - 1);
            int y2 = std::min(item.box[3], (int)img.height - 1);
            if (x1 >= x2 || y1 >= y2) {
                continue;
            }
            std::vector<uint8_t> color = item.color;
            if (is_rgb565) {
#if CONFIG_IDF_TARGET_ESP32P4
                dl::image::pix_type_t pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
#else
                dl::image::pix_type_t pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;
#endif
                std::vector<uint8_t> color565(2);
                dl::image::cvt_pix(color.data(), color565.data(), dl::image::DL_IMAGE_PIX_TYPE_RGB888, pix_type);
                color = color565;
            }
            dl::image::draw_hollow_rectangle(img, x1, y1, x2, y2, color, 2);
            for (size_t i = 0; i + 1 < item.keypoint.size(); i += 2) {
                if (item.keypoint[i] >= 0 && item.keypoint[i] < (int)img.width && item.keypoint[i + 1] >= 0 &&
                    item.keypoint[i + 1] < (int)img.height) {
                    dl::image::draw_point(img, item.keypoint[i], item.keypoint[i + 1], color, 3);
                }
            }
            if (item.cross) {
                int cx = (x1 + x2) / 2;
                int cy = (y1 + y2) / 2;
                static constexpr int kCrossHalfLen = 6;
                dl::image::draw_point(img, cx, cy, color, 2);
                dl::image::draw_point(img, cx - kCrossHalfLen, cy, color, 2);
                dl::image::draw_point(img, cx + kCrossHalfLen, cy, color, 2);
                dl::image::draw_point(img, cx, cy - kCrossHalfLen, color, 2);
                dl::image::draw_point(img, cx, cy + kCrossHalfLen, color, 2);
            }
            // Text labels need a font renderer, only supported on the LVGL canvas.
        }
    }
#else
    draw_items_on_canvas(m_canvas, m_result.second);
#endif
}

void WhoDetectResultLCDDisp::cleanup()
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    std::queue<std::pair<int64_t, std::vector<draw_item_t>>>().swap(m_results);
    m_result = {};
    xSemaphoreGive(m_res_mutex);
}
} // namespace lcd_disp
} // namespace who
