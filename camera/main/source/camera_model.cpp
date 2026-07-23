/**
 * @file camera_model.cpp
 * @brief 端侧目标检测 — 使用 ESP-DL CatDetect（espdet_pico_224_224_cat）。
 *
 * 预处理：RGB565 → RGB888 → ImagePreprocessor（letterbox + resize）
 * 推理+后处理：CatDetect::run() 返回检测框列表
 * 坐标映射：模型坐标 → 240×240 摄像头原始帧
 */

#include "camera_model.h"
#include "camera_model.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#if !defined(CAMERA_MODEL_SKIP)

#include "cat_detect.hpp"
#include "dl_image.hpp"

static const char *TAG = "cam_model";

extern const uint8_t cat_detect_espdl[] asm("_binary_espdet_pico_espdl_start");

static CatDetect              *s_cat_detect  = NULL;
static dl::image::img_t       s_input_img;
static camera_detection_result_t s_result;

static float s_scale_x = 1.0f;
static float s_scale_y = 1.0f;

/* ---- RGB565 → RGB888 ---- */
static void rgb565_to_rgb888(const uint8_t *src, uint8_t *dst,
                              uint16_t w, uint16_t h)
{
    uint32_t n = (uint32_t)w * (uint32_t)h;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t px = (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
        src += 2;
        dst[0] = (uint8_t)(((px >> 11) & 0x1FU) << 3);
        dst[1] = (uint8_t)(((px >> 5) & 0x3FU) << 2);
        dst[2] = (uint8_t)((px & 0x1FU) << 3);
        dst += 3;
    }
}

/* ==========================================================================
 * 公开 API
 * ========================================================================== */

status_t camera_model_init(void)
{
    if (s_cat_detect != NULL) return STATUS_OK;

    s_cat_detect = new CatDetect(CatDetect::ESPDET_PICO_224_224_CAT, false);
    if (s_cat_detect == NULL) {
        ESP_LOGE(TAG, "CatDetect alloc failed");
        return STATUS_FAIL;
    }

    s_input_img.data   = NULL;
    s_input_img.width  = CAMERA_MODEL_CAM_W;
    s_input_img.height = CAMERA_MODEL_CAM_H;
    s_input_img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    s_scale_x = 1.0f;
    s_scale_y = 1.0f;

    ESP_LOGI(TAG, "CatDetect initialized (224x224 cat)");
    return STATUS_OK;
}

const camera_detection_result_t *camera_model_detect(const uint8_t *rgb565,
                                                      uint16_t width,
                                                      uint16_t height,
                                                      uint32_t frame_id)
{
    memset(&s_result, 0, sizeof(s_result));
    s_result.frame_id = frame_id;

    if (s_cat_detect == NULL || rgb565 == NULL) return &s_result;
    if (width != CAMERA_MODEL_CAM_W || height != CAMERA_MODEL_CAM_H)
        return &s_result;

    int64_t t0 = esp_timer_get_time();

    /* RGB565 → RGB888 */
    size_t rgb888_size = (size_t)width * (size_t)height * 3;
    if (s_input_img.data == NULL) {
        s_input_img.data = heap_caps_aligned_alloc(16, rgb888_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_input_img.data == NULL) return &s_result;
    }
    rgb565_to_rgb888(rgb565, (uint8_t *)s_input_img.data, width, height);

    /* 推理+后处理 */
    auto &results = s_cat_detect->run(s_input_img);

    /* 映射坐标 */
    s_result.count = 0;
    for (const auto &r : results) {
        if (s_result.count >= CAMERA_MODEL_MAX_BOXES) break;

        camera_detection_t *d = &s_result.boxes[s_result.count];
        d->x          = (uint16_t)(r.box[0] * s_scale_x);
        d->y          = (uint16_t)(r.box[1] * s_scale_y);
        d->w          = (uint16_t)((r.box[2] - r.box[0]) * s_scale_x);
        d->h          = (uint16_t)((r.box[3] - r.box[1]) * s_scale_y);
        d->class_id   = (uint8_t)r.category;
        d->confidence = r.score;
        s_result.count++;
    }

    s_result.elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (s_result.count > 0) {
        ESP_LOGI(TAG, "frame %lu: %u detections, %lums",
                 (unsigned long)frame_id, s_result.count,
                 (unsigned long)s_result.elapsed_ms);
    }
    return &s_result;
}

void camera_model_deinit(void)
{
    if (s_cat_detect) { delete s_cat_detect; s_cat_detect = NULL; }
    if (s_input_img.data) { heap_caps_free(s_input_img.data); s_input_img.data = NULL; }
}

bool_t camera_model_is_ready(void)
{
    return (s_cat_detect != NULL) ? TRUE : FALSE;
}

#else /* CAMERA_MODEL_SKIP */

static const char *TAG = "cam_model";
static camera_detection_result_t s_result;

status_t camera_model_init(void)
{
    ESP_LOGW(TAG, "model skipped (CAMERA_MODEL_SKIP defined)");
    return STATUS_FAIL;
}

const camera_detection_result_t *camera_model_detect(const uint8_t *rgb565,
                                                      uint16_t width,
                                                      uint16_t height,
                                                      uint32_t frame_id)
{
    (void)rgb565; (void)width; (void)height;
    s_result.count = 0;
    s_result.frame_id = frame_id;
    return &s_result;
}

void camera_model_deinit(void) {}
bool_t camera_model_is_ready(void) { return FALSE; }

#endif /* CAMERA_MODEL_SKIP */
