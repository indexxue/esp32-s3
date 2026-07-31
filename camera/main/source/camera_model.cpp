/**
 * @file camera_model.cpp
 * @brief BallDetect 推理任务与最新结果缓存（含叠框运动外推）。
 */

#include "camera_model.h"

#include <cmath>
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
/** 略高于 JPEG/跟球，预览常开时少被图传饿死。 */
#define CAMERA_MODEL_TASK_PRIO (5U)
/** 目标检测周期下限；infer 更长时跑完即下一帧。 */
#define CAMERA_MODEL_INTERVAL_MS (80U)
#define CAMERA_MODEL_MIN_GAP_MS (40U)
#define CAMERA_MODEL_BOX_COLOR LCD_COLOR_GREEN
/** 叠框外推上限（us）；超过则冻结在最近预测，避免乱飞。 */
#define CAMERA_MODEL_PRED_MAX_AGE_US (400000LL)
/** 速度低通：new = a*meas + (1-a)*old */
#define CAMERA_MODEL_VEL_ALPHA (0.55f)

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

/** 最近一次结果时间戳，以及各框中心速度（像素/秒）。 */
static int64_t s_result_ts_us;
static float s_vx[CAMERA_MODEL_MAX_BOXES];
static float s_vy[CAMERA_MODEL_MAX_BOXES];
static camera_model_result_t s_prev_result;
static int64_t s_prev_ts_us;

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

static void camera_model_update_velocity(const camera_model_result_t *cur, int64_t now_us)
{
    uint8_t i;

    if ((cur == NULL) || (cur->count == 0U) || (s_result_ts_us <= 0) || (s_latest.count == 0U)) {
        for (i = 0U; i < CAMERA_MODEL_MAX_BOXES; i++) {
            s_vx[i] = 0.0f;
            s_vy[i] = 0.0f;
        }
        return;
    }

    const float dt_s = (float)(now_us - s_result_ts_us) * 1.0e-6f;
    if (dt_s < 0.02f) {
        return;
    }

    /* 单类钢珠：按序号对应（通常 count=1）。 */
    const uint8_t n = (cur->count < s_latest.count) ? cur->count : s_latest.count;
    for (i = 0U; i < n; i++) {
        const camera_model_box_t *a = &s_latest.boxes[i];
        const camera_model_box_t *b = &cur->boxes[i];
        const float cx0 = (float)a->x + (float)a->w * 0.5f;
        const float cy0 = (float)a->y + (float)a->h * 0.5f;
        const float cx1 = (float)b->x + (float)b->w * 0.5f;
        const float cy1 = (float)b->y + (float)b->h * 0.5f;
        const float mvx = (cx1 - cx0) / dt_s;
        const float mvy = (cy1 - cy0) / dt_s;
        s_vx[i] = CAMERA_MODEL_VEL_ALPHA * mvx + (1.0f - CAMERA_MODEL_VEL_ALPHA) * s_vx[i];
        s_vy[i] = CAMERA_MODEL_VEL_ALPHA * mvy + (1.0f - CAMERA_MODEL_VEL_ALPHA) * s_vy[i];
    }
    for (; i < CAMERA_MODEL_MAX_BOXES; i++) {
        s_vx[i] = 0.0f;
        s_vy[i] = 0.0f;
    }
}

static void camera_model_store_results(const std::list<dl::detect::result_t> &results, uint16_t w, uint16_t h)
{
    camera_model_result_t tmp = {};
    const int64_t now_us = esp_timer_get_time();
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
    camera_model_update_velocity(&tmp, now_us);
    s_prev_result = s_latest;
    s_prev_ts_us = s_result_ts_us;
    s_latest = tmp;
    s_result_ts_us = now_us;
    s_result_gen++;
    camera_model_result_unlock();
}

static void camera_model_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(CAMERA_MODEL_INTERVAL_MS);
    const TickType_t min_gap = pdMS_TO_TICKS(CAMERA_MODEL_MIN_GAP_MS);
    uint32_t run_count = 0U;

    (void)arg;

    for (;;) {
        uint16_t w = 0U;
        uint16_t h = 0U;
        uint32_t gen = 0U;
        const uint8_t *borrowed_ptr = NULL;
        bool_t borrowed = FALSE;
        int64_t t0;
        int64_t t1;
        TickType_t t_start;
        TickType_t elapsed;

        if ((s_detect == NULL) || (s_frame_buf == NULL) || (s_enabled == FALSE)) {
            vTaskDelay(period);
            continue;
        }

        t_start = xTaskGetTickCount();

        /*
         * 先借帧立刻拷贝再释放：推理可长达数百 ms，不能一直占住 snapshot 槽，
         * 否则预览/JPEG 只能写另一槽，且叠框用的结果会更旧。
         */
        if (camera_sensor_acquire_rgb565(&borrowed_ptr, &w, &h, &gen) == STATUS_OK) {
            const uint32_t frame_bytes = (uint32_t)w * (uint32_t)h * 2U;
            if ((frame_bytes > 0U) && (frame_bytes <= s_frame_cap) && (borrowed_ptr != NULL)) {
                (void)memcpy(s_frame_buf, borrowed_ptr, (size_t)frame_bytes);
                borrowed = TRUE;
            }
            camera_sensor_release_rgb565();
            if (borrowed == FALSE) {
                vTaskDelay(min_gap);
                continue;
            }
        } else if (camera_sensor_copy_rgb565(s_frame_buf, s_frame_cap, &w, &h) != STATUS_OK) {
            vTaskDelay(min_gap);
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
            LOG_INFO("%s infer %lld us boxes=%u %ux%u gen=%u",
                     TAG,
                     (long long)(t1 - t0),
                     (unsigned)results.size(),
                     (unsigned)w,
                     (unsigned)h,
                     (unsigned)gen);
            if (results.size() > 0U) {
                uint8_t i = 0U;
                for (const auto &res : results) {
                    if (i >= CAMERA_MODEL_MAX_BOXES) {
                        break;
                    }
                    if (res.box.size() < 4U) {
                        continue;
                    }
                    const int x1 = res.box[0];
                    const int y1 = res.box[1];
                    const int x2 = res.box[2];
                    const int y2 = res.box[3];
                    const uint16_t bw = (uint16_t)(x2 - x1);
                    const uint16_t bh = (uint16_t)(y2 - y1);
                    const uint16_t cx = (uint16_t)(x1 + (bw / 2));
                    const uint16_t cy = (uint16_t)(y1 + (bh / 2));
                    LOG_INFO("%s box%u center=(%u,%u) xywh=%u,%u,%u,%u score=%.2f frame=%ux%u",
                             TAG,
                             (unsigned)i,
                             (unsigned)cx,
                             (unsigned)cy,
                             (unsigned)((x1 < 0) ? 0 : x1),
                             (unsigned)((y1 < 0) ? 0 : y1),
                             (unsigned)bw,
                             (unsigned)bh,
                             (double)res.score,
                             (unsigned)w,
                             (unsigned)h);
                    i++;
                }
            }
        }

        /* 跑完即下一帧；仅在 infer 快于目标周期时补齐间隔。 */
        elapsed = xTaskGetTickCount() - t_start;
        if (elapsed < period) {
            vTaskDelay(period - elapsed);
        } else {
            vTaskDelay(1);
        }
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
    (void)memset(&s_prev_result, 0, sizeof(s_prev_result));
    (void)memset(s_vx, 0, sizeof(s_vx));
    (void)memset(s_vy, 0, sizeof(s_vy));
    s_result_ts_us = 0;
    s_prev_ts_us = 0;

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
            (void)memset(s_vx, 0, sizeof(s_vx));
            (void)memset(s_vy, 0, sizeof(s_vy));
            s_result_ts_us = 0;
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
    float vx_local[CAMERA_MODEL_MAX_BOXES];
    float vy_local[CAMERA_MODEL_MAX_BOXES];
    int64_t result_ts;
    int64_t now_us;
    int64_t age_us;
    uint8_t n;
    uint8_t i;

    if ((out_flat == NULL) || (out_count == NULL) || (max_count == 0U)) {
        return STATUS_INVALID_ARG;
    }

    if (camera_model_result_lock(pdMS_TO_TICKS(20)) != STATUS_OK) {
        *out_count = 0U;
        return STATUS_TIMEOUT;
    }
    latest = s_latest;
    result_ts = s_result_ts_us;
    (void)memcpy(vx_local, s_vx, sizeof(vx_local));
    (void)memcpy(vy_local, s_vy, sizeof(vy_local));
    camera_model_result_unlock();

    n = latest.count;
    if (n > max_count) {
        n = max_count;
    }
    if (n > CAMERA_MODEL_MAX_BOXES) {
        n = CAMERA_MODEL_MAX_BOXES;
    }

    now_us = esp_timer_get_time();
    age_us = (result_ts > 0) ? (now_us - result_ts) : 0;
    if (age_us < 0) {
        age_us = 0;
    }
    if (age_us > CAMERA_MODEL_PRED_MAX_AGE_US) {
        age_us = CAMERA_MODEL_PRED_MAX_AGE_US;
    }
    const float age_s = (float)age_us * 1.0e-6f;

    for (i = 0U; i < n; i++) {
        uint16_t *dst = out_flat + ((uint32_t)i * CAMERA_MODEL_UI_BOX_STRIDE);
        float cx = (float)latest.boxes[i].x + (float)latest.boxes[i].w * 0.5f;
        float cy = (float)latest.boxes[i].y + (float)latest.boxes[i].h * 0.5f;
        float half_w = (float)latest.boxes[i].w * 0.5f;
        float half_h = (float)latest.boxes[i].h * 0.5f;
        int x1;
        int y1;
        int x2;
        int y2;

        /* 用框中心速度把结果推到「现在」，补偿 infer 延迟导致的叠框滞后。 */
        cx += vx_local[i] * age_s;
        cy += vy_local[i] * age_s;

        x1 = (int)lroundf(cx - half_w);
        y1 = (int)lroundf(cy - half_h);
        x2 = (int)lroundf(cx + half_w);
        y2 = (int)lroundf(cy + half_h);

        if (latest.frame_w > 0U) {
            if (x1 < 0) {
                x1 = 0;
            }
            if (y1 < 0) {
                y1 = 0;
            }
            if (x2 > (int)latest.frame_w) {
                x2 = (int)latest.frame_w;
            }
            if (y2 > (int)latest.frame_h) {
                y2 = (int)latest.frame_h;
            }
        }
        if (x2 <= x1) {
            x2 = x1 + 1;
        }
        if (y2 <= y1) {
            y2 = y1 + 1;
        }

        dst[0] = (uint16_t)x1;
        dst[1] = (uint16_t)y1;
        dst[2] = (uint16_t)(x2 - x1);
        dst[3] = (uint16_t)(y2 - y1);
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
