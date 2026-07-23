/**
 * @file start.c
 * @brief camera 平台壳层：log、NVS、板级、单键、Web/OTA、目标检测推理（可宏关闭）。
 */

#include "start.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_ui.h"
#include "camera_sensor.h"
#include "camera_model.h"
#include "web_pages.h"

#include "type.h"

#include "device_profile.h"
#include "log.h"
#include "nvs.h"
#include "board.h"
#include "button.h"
#include "flexible_button.h"
#include "led_scene.h"
#include "ota.h"

#if CONFIG_WEB_CTRL_AUTO_START
#include "esp_err.h"
#include "web_ctrl.h"
#include "web_server.h"

#define WEB_CTRL_BOOT_TASK_STACK_WORDS (10240U)
#define WEB_CTRL_BOOT_TASK_PRIORITY (3U)

static void web_ctrl_boot_task(void *arg)
{
    web_ctrl_config_t wcfg;

    (void)arg;
    web_ctrl_config_init_defaults(&wcfg);
    web_ctrl_config_merge_nvs(&wcfg);
    wcfg.root_get_handler = web_pages_root_get_handler;
    const esp_err_t werr = web_ctrl_start(&wcfg);
    if (werr != ESP_OK) {
        LOG_WARN("web_ctrl_start failed: %s", esp_err_to_name(werr));
    } else {
        const esp_err_t reg = web_pages_register(web_server_get_handle());
        if (reg != ESP_OK) {
            LOG_WARN("web_pages_register failed: %s", esp_err_to_name(reg));
        }
        const status_t ota_st = ota_confirm_running_image();
        if (ota_st != STATUS_OK) {
            LOG_WARN("ota_confirm_running_image: %s", status_to_str(ota_st));
        }
    }
    vTaskDelete(NULL);
}
#endif

#define BUTTON_SCAN_PERIOD_MS (1000 / FLEX_BTN_SCAN_FREQ_HZ)
#define BTN_SCAN_TASK_STACK_WORDS (3072U)
#define BTN_SCAN_TASK_PRIORITY (5U)

#define CAMERA_MODULES_TASK_STACK_WORDS (16384U)
#define CAMERA_MODULES_TASK_PRIORITY (4U)

/** 检测框 LCD 绘制 buffer（每框 5 个 uint16: x, y, w, h, color_565）。 */
#define CAMERA_DETECT_DRAW_BUF_SIZE (CAMERA_MODEL_MAX_BOXES * 5U)

static void app_idle_default(void)
{
    const TickType_t period = pdMS_TO_TICKS(APP_LIFECYCLE_IDLE_DELAY_MS);

    for (;;) {
        vTaskDelay(period);
    }
}

status_t app_start(const app_lifecycle_t *lifecycle)
{
    status_t err;

    if (lifecycle == NULL) {
        return STATUS_INVALID_ARG;
    }

    if (lifecycle->init != NULL) {
        err = lifecycle->init();
        if (err != STATUS_OK) {
            return err;
        }
    }

    if (lifecycle->run != NULL) {
        lifecycle->run();
        return STATUS_OK;
    }

    app_idle_default();
    return STATUS_OK;
}

static void app_button_notify(btn_id_e id, const char *name, btn_permission_e permission, btn_event_e event)
{
    st7789_t *lcd = BoardSt7789();

    (void)permission;
    LOG_INFO("key %s (%s): %s", button_id_to_str(id), (name != NULL) ? name : "?", button_event_to_str(event));

    if (event == BTN_EVENT_LONG_PRESS) {
        LOG_INFO("button long press");
        return;
    }

    if (event != BTN_EVENT_SINGLE_CLICK) {
        return;
    }

    if (id == BTN_ID_CONFIRM) {
        if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED)) {
            led_scene_run(LED_SCENE_ID_TRIGGER);
        }
        if (st7789_is_initialized(lcd)) {
            camera_ui_set_status_line(lcd, "key: pressed");
        }
    }
}

static void button_scan_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(BUTTON_SCAN_PERIOD_MS);

    for (;;) {
        button_schedule();
        vTaskDelay(period);
    }
}

static void camera_modules_task(void *arg)
{
    st7789_t *lcd = BoardSt7789();
    uint32_t frame_id = 0U;
    uint16_t draw_buf[CAMERA_DETECT_DRAW_BUF_SIZE];

    (void)arg;

    /* ---- LCD UI ---- */
    if (st7789_is_initialized(lcd)) {
        (void)camera_ui_init(lcd);
    }

    /* ---- 摄像头 ---- */
    if (camera_sensor_init() == STATUS_OK) {
        if (camera_sensor_preview_start() != STATUS_OK) {
            LOG_WARN("camera sensor preview start failed");
            if (st7789_is_initialized(lcd)) {
                camera_ui_set_status_line(lcd, "cam: preview fail");
            }
        }
    } else {
        if (st7789_is_initialized(lcd)) {
            camera_ui_set_status_line(lcd, "cam: init fail");
        }
        LOG_WARN("camera_sensor_init failed");
    }

    /* ---- 模型（CMakeLists.txt CAMERA_MODEL_ENABLE=1 恢复推理） ---- */
#ifdef CAMERA_MODEL_SKIP
    const bool model_ready = false;
    LOG_INFO("model disabled (CAMERA_MODEL_ENABLE=0)");
    if (st7789_is_initialized(lcd)) {
        camera_ui_set_status_line(lcd, "model: off");
    }
#else
    bool model_ready = (camera_model_init() == STATUS_OK);
    if (model_ready) {
        LOG_INFO("model ready");
        if (st7789_is_initialized(lcd)) {
            camera_ui_set_status_line(lcd, "model: ready");
        }
    } else {
        LOG_WARN("camera_model_init failed");
        if (st7789_is_initialized(lcd)) {
            camera_ui_set_status_line(lcd, "model: no file");
        }
    }
#endif

    /* ---- 主循环：预览 + 检测 ---- */
    for (;;) {
        const uint8_t *snap = camera_sensor_get_snapshot();
        if (snap == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (model_ready) {
            const camera_detection_result_t *res =
                camera_model_detect(snap, CAMERA_MODEL_CAM_W, CAMERA_MODEL_CAM_H, frame_id);
            if (res != NULL && res->count > 0) {
                /* 填入 draw_buf 并送 LCD */
                uint8_t n = res->count;
                if (n > CAMERA_MODEL_MAX_BOXES) n = CAMERA_MODEL_MAX_BOXES;
                for (uint8_t i = 0; i < n; i++) {
                    const camera_detection_t *d = &res->boxes[i];
                    uint16_t color = (d->confidence >= 0.70f) ? 0x07E0   /* 绿 */
                                   : (d->confidence >= 0.50f) ? 0xFFE0   /* 黄 */
                                   : 0xF800;                             /* 红 */
                    uint16_t *p = &draw_buf[i * 5U];
                    p[0] = d->x;
                    p[1] = d->y;
                    p[2] = d->w;
                    p[3] = d->h;
                    p[4] = color;
                }
                if (st7789_is_initialized(lcd)) {
                    camera_ui_draw_detection_boxes(lcd, draw_buf, n,
                                                   CAMERA_MODEL_CAM_W, CAMERA_MODEL_CAM_H);
                }

                /* 串口输出 */
                printf("[DETECT] frame=%lu boxes=%u time=%lums\n",
                       (unsigned long)res->frame_id, res->count,
                       (unsigned long)res->elapsed_ms);
                for (uint8_t i = 0; i < n; i++) {
                    const camera_detection_t *d = &res->boxes[i];
                    printf("  #%u: cls=%u conf=%.2f xywh=(%u,%u,%u,%u)\n",
                           i, d->class_id, (double)d->confidence,
                           d->x, d->y, d->w, d->h);
                }

                char buf[32];
                snprintf(buf, sizeof(buf), "det:%u %lums", res->count,
                         (unsigned long)res->elapsed_ms);
                if (st7789_is_initialized(lcd)) {
                    camera_ui_set_status_line(lcd, buf);
                }
            }
            frame_id++;
        }

        vTaskDelay(pdMS_TO_TICKS(CAMERA_UI_DETECT_REFRESH_MS));
    }
}

status_t camera_start_modules_task(void)
{
    if (xTaskCreate(camera_modules_task, "cam_mod", CAMERA_MODULES_TASK_STACK_WORDS, NULL,
                    CAMERA_MODULES_TASK_PRIORITY, NULL) != pdPASS) {
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

static status_t app_init_platform(void)
{
    if (log_init(NULL) != STATUS_OK) {
        return STATUS_FAIL;
    }

    nvs_init();

    if (BoardInit() != STATUS_OK) {
        LOG_ERROR("BoardInit failed");
        return STATUS_FAIL;
    }

#if CONFIG_WEB_CTRL_AUTO_START
    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_WEB)) {
        if (xTaskCreate(web_ctrl_boot_task, "web_boot", WEB_CTRL_BOOT_TASK_STACK_WORDS, NULL,
                        WEB_CTRL_BOOT_TASK_PRIORITY, NULL) != pdPASS) {
            LOG_WARN("create web_boot task failed");
        }
    }
#endif

    return STATUS_OK;
}

static status_t app_init_button_io(void)
{
    button_init(app_button_notify);

    if (xTaskCreate(button_scan_task, "btn_scan", BTN_SCAN_TASK_STACK_WORDS, NULL, BTN_SCAN_TASK_PRIORITY, NULL) !=
        pdPASS) {
        LOG_ERROR("create btn_scan task failed");
        return STATUS_FAIL;
    }

    LOG_INFO("camera: 1 button GPIO0, scan %d Hz", FLEX_BTN_SCAN_FREQ_HZ);
    return STATUS_OK;
}

static status_t app_init_led_ui(void)
{
    status_t err = led_scene_init();
    if (err != STATUS_OK) {
        LOG_ERROR("led_scene_init failed: %s", status_to_str(err));
        return err;
    }

    err = led_scene_start_update_task();
    if (err != STATUS_OK) {
        LOG_ERROR("led_scene_start_update_task failed: %s", status_to_str(err));
        return err;
    }

    led_scene_run(LED_SCENE_ID_BOOTUP);
    LOG_INFO("WS2812 RGB on GPIO%d x%u", BOARD_CAMERA_WS2812_PIN, (unsigned)BOARD_CAMERA_WS2812_COUNT);
    return STATUS_OK;
}

static status_t app_init(void)
{
    status_t err = app_init_platform();
    const device_product_profile_t *product = device_profile_product();

    if (err != STATUS_OK) {
        return err;
    }

    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_BUTTON)) {
        err = app_init_button_io();
        if (err != STATUS_OK) {
            return err;
        }
    }

    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED)) {
        err = app_init_led_ui();
        if (err != STATUS_OK) {
            return err;
        }
    }

    LOG_INFO("%s ready (platform_mask=0x%02lX)", product->name, (unsigned long)device_profile_platform_mask());
    return STATUS_OK;
}

static void app_run(void)
{
    if (camera_start_modules_task() != STATUS_OK) {
        LOG_ERROR("camera_start_modules_task failed");
    }

    app_idle_default();
}

static const app_lifecycle_t s_default_lifecycle = {
    .init = app_init,
    .run  = app_run,
};

status_t app_entry(void)
{
    return app_start(&s_default_lifecycle);
}
