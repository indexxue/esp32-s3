/**
 * @file camera_model.h
 * @brief 钢珠 ESPDet 检测封装：结果缓存 + LCD 画框数据。
 */

#pragma once

#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CAMERA_MODEL_MAX_BOXES
#define CAMERA_MODEL_MAX_BOXES (8U)
#endif

/** UI 扁平框：每框 5×uint16 = [x, y, w, h, color_565] */
#define CAMERA_MODEL_UI_BOX_STRIDE (5U)

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    float    score;
    int32_t  category;
} camera_model_box_t;

typedef struct {
    uint8_t  count;
    uint16_t frame_w;
    uint16_t frame_h;
    camera_model_box_t boxes[CAMERA_MODEL_MAX_BOXES];
} camera_model_result_t;

status_t camera_model_init(void);
status_t camera_model_start(void);
bool_t camera_model_is_ready(void);

/** 拷贝最近一次检测结果。 */
status_t camera_model_get_latest(camera_model_result_t *out);

/**
 * 填充 LCD 画框用扁平数组。
 * @param out_flat  长度至少 max_count * 5
 * @param max_count 最多框数
 * @param out_count 实际框数
 * @param out_w/h   结果所在帧尺寸（与 snapshot 一致）
 */
status_t camera_model_fill_ui_boxes(uint16_t *out_flat,
                                    uint8_t max_count,
                                    uint8_t *out_count,
                                    uint16_t *out_w,
                                    uint16_t *out_h);

#ifdef __cplusplus
}
#endif
