/**
 * @file camera_model.cpp
 * @brief BallDetect 推理任务与最新结果缓存。
 */

#include "camera_model.h"

#include <cstring>
#include <new>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "camera_sensor.h"
#include "espdet_detect.hpp"
#include "model_stamp.h"
#include "dl_image_define.hpp"
#include "log.h"
#include "lcd.h"

#define CAMERA_MODEL_TASK_STACK (24576U)
#define CAMERA_MODEL_TASK_PRIO (3U)
#define CAMERA_MODEL_INTERVAL_MS (250U)
#define CAMERA_MODEL_BOX_COLOR LCD_COLOR_GREEN

static const char *TAG = "cam_model";

static BallDetect *s_detect;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_result_mtx;
static camera_model_result_t s_latest;
static uint8_t *s_frame_buf;
static uint32_t s_frame_cap;
static bool_t s_ready;
static bool_t s_enabled = TRUE;
static uint32_t s_result_gen;

static status_t camera_model_result_lock(TickType_t ticks)
{
    if (s_result_mtx == NULL) {
        return STATUS_FAIL;
    }
    return (xSemaphoreTake(s_result_mtx, ticks) == pdTRUE) ? STATUS_OK : STATUS_TIMEOUT;
}

static void camera_model_result_unlock(void)
{
    if (s_result_mtx != NULL) {
        (void)xSemaphoreGive(s_result_mtx);
    }
}

static void camera_model_store_results(const std::list<dl::detect::result_t> &results, uint16_t w, uint16_t h)
{
    camera_model_result_t tmp = {};
    tmp.frame_w = w;
    tmp.frame_h = h;

    for (const auto &res : results) {
        if (tmp.count >= CAMERA_MODEL_MAX_BOXES) {
            break;
        }
        if (res.box.size() < 4U) {
            continue;
        }
        int x1 = res.box[0];
        int y1 = res.box[1];
        int x2 = res.box[2];
        int y2 = res.box[3];
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }
        camera_model_box_t *b = &tmp.boxes[tmp.count];
        b->x = (uint16_t)((x1 < 0) ? 0 : x1);
        b->y = (uint16_t)((y1 < 0) ? 0 : y1);
        b->w = (uint16_t)(x2 - x1);
        b->h = (uint16_t)(y2 - y1);
        b->score = res.score;
        b->category = res.category;
        tmp.count++;
    }

    if (camera_model_result_lock(pdMS_TO_TICKS(50)) != STATUS_OK) {
        return;
    }
    s_latest = tmp;
    s_result_gen++;
    camera_model_result_unlock();
}

static void camera_model_task(void *arg)
{
    const TickType_t interval = pdMS_TO_TICKS(CAMERA_MODEL_INTERVAL_MS);
    uint32_t run_count = 0U;

    (void)arg;

    for (;;) {
        uint16_t w = 0U;
        uint16_t h = 0U;
        int64_t t0;
        int64_t t1;

        if ((s_detect == NULL) || (s_frame_buf == NULL) || (s_enabled == FALSE)) {
            vTaskDelay(interval);
            continue;
        }

        if (camera_sensor_copy_rgb565(s_frame_buf, s_frame_cap, &w, &h) != STATUS_OK) {
            vTaskDelay(interval);
            continue;
        }

        dl::image::img_t img = {};
        img.data = s_frame_buf;
        img.width = w;
        img.height = h;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;

        t0 = esp_timer_get_time();
        auto &results = s_detect->run(img);
        t1 = esp_timer_get_time();
        camera_model_store_results(results, w, h);

        run_count++;
        if ((run_count == 1U) || ((run_count % 20U) == 0U)) {
            LOG_INFO("%s infer %lld us boxes=%u %ux%u",
                     TAG,
                     (long long)(t1 - t0),
                     (unsigned)results.size(),
                     (unsigned)w,
                     (unsigned)h);
        }

        vTaskDelay(interval);
    }
}

extern "C" status_t camera_model_init(void)
{
    if (s_ready != FALSE) {
        return STATUS_OK;
    }

    s_result_mtx = xSemaphoreCreateMutex();
    if (s_result_mtx == NULL) {
        return STATUS_NO_MEM;
    }
    (void)memset(&s_latest, 0, sizeof(s_latest));

    /* snapshot 最大为采集分辨率 RGB565 */
    s_frame_cap = (uint32_t)CAMERA_SENSOR_CAPTURE_WIDTH * (uint32_t)CAMERA_SENSOR_CAPTURE_HEIGHT * 2U;

    s_frame_buf = (uint8_t *)heap_caps_malloc(s_frame_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_frame_buf == NULL) {
        s_frame_buf = (uint8_t *)heap_caps_malloc(s_frame_cap, MALLOC_CAP_8BIT);
    }
    if (s_frame_buf == NULL) {
        LOG_ERROR("%s frame buf alloc failed (%u)", TAG, (unsigned)s_frame_cap);
        return STATUS_NO_MEM;
    }

    s_detect = new (std::nothrow) BallDetect();
    if (s_detect == NULL) {
        LOG_ERROR("%s BallDetect alloc failed", TAG);
        return STATUS_NO_MEM;
    }

    s_ready = TRUE;
    LOG_INFO("%s init ok (espdet ball stamp=%s note=%s)",
             TAG,
             BALL_DETECT_MODEL_STAMP,
             BALL_DETECT_MODEL_NOTE);
    return STATUS_OK;
}

extern "C" status_t camera_model_start(void)
{
    if (s_ready == FALSE) {
        status_t st = camera_model_init();
        if (st != STATUS_OK) {
            return st;
        }
    }
    if (s_task != NULL) {
        return STATUS_OK;
    }
    if (xTaskCreate(camera_model_task,
                    "cam_detect",
                    CAMERA_MODEL_TASK_STACK,
                    NULL,
                    CAMERA_MODEL_TASK_PRIO,
                    &s_task) != pdPASS) {
        LOG_ERROR("%s task create failed", TAG);
        s_task = NULL;
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

extern "C" bool_t camera_model_is_ready(void)
{
    return s_ready;
}

extern "C" status_t camera_model_set_enabled(bool_t on)
{
    s_enabled = (on != FALSE) ? TRUE : FALSE;

    /* SPI 可能早于 model_init 启动；无 mutex 时只记开关。 */
    if (s_result_mtx != NULL) {
        if (camera_model_result_lock(pdMS_TO_TICKS(50)) != STATUS_OK) {
            return STATUS_TIMEOUT;
        }
        if (s_enabled == FALSE) {
            (void)memset(&s_latest, 0, sizeof(s_latest));
            s_result_gen++;
        }
        camera_model_result_unlock();
    }

    LOG_INFO("%s detect %s", TAG, (s_enabled != FALSE) ? "ON" : "OFF");
    return STATUS_OK;
}

extern "C" bool_t camera_model_is_enabled(void)
{
    return s_enabled;
}

extern "C" status_t camera_model_get_latest_ex(camera_model_result_t *out, uint32_t *gen)
{
    if (out == NULL) {
        return STATUS_INVALID_ARG;
    }
    if (camera_model_result_lock(pdMS_TO_TICKS(20)) != STATUS_OK) {
        return STATUS_TIMEOUT;
    }
    *out = s_latest;
    if (gen != NULL) {
        *gen = s_result_gen;
    }
    camera_model_result_unlock();
    return STATUS_OK;
}

extern "C" status_t camera_model_get_latest(camera_model_result_t *out)
{
    return camera_model_get_latest_ex(out, NULL);
}

extern "C" status_t camera_model_fill_ui_boxes(uint16_t *out_flat,
                                               uint8_t max_count,
                                               uint8_t *out_count,
                                               uint16_t *out_w,
                                               uint16_t *out_h)
{
    camera_model_result_t latest;
    uint8_t n;
    uint8_t i;

    if ((out_flat == NULL) || (out_count == NULL) || (max_count == 0U)) {
        return STATUS_INVALID_ARG;
    }
    if (camera_model_get_latest(&latest) != STATUS_OK) {
        *out_count = 0U;
        return STATUS_FAIL;
    }

    n = latest.count;
    if (n > max_count) {
        n = max_count;
    }
    for (i = 0U; i < n; i++) {
        uint16_t *dst = out_flat + ((uint32_t)i * CAMERA_MODEL_UI_BOX_STRIDE);
        dst[0] = latest.boxes[i].x;
        dst[1] = latest.boxes[i].y;
        dst[2] = latest.boxes[i].w;
        dst[3] = latest.boxes[i].h;
        dst[4] = (uint16_t)CAMERA_MODEL_BOX_COLOR;
    }
    *out_count = n;
    if (out_w != NULL) {
        *out_w = latest.frame_w;
    }
    if (out_h != NULL) {
        *out_h = latest.frame_h;
    }
    return STATUS_OK;
}
