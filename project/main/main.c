/*
 * @Author: indexxue 2308039918@qq.com
 * @Date: 2026-04-30 21:58:22
 * @LastEditors: indexxue 2308039918@qq.com
 * @LastEditTime: 2026-05-02 10:16:34
 * @FilePath: \ESP32-S3\project\main\main.c
 * @Description: 应用入口与板级演示逻辑（按键场景、LED 场景）。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "type.h"

#include "log.h"
#include "board.h"
#include "button.h"
#include "flexible_button.h"
#include "led_scene.h"
#include "start.h"

/* ---------- 本文件内可调参数 ---------- */

#define BUTTON_SCAN_PERIOD_MS (1000 / FLEX_BTN_SCAN_FREQ_HZ)

#define BTN_SCAN_TASK_STACK_WORDS (3072U)
#define BTN_SCAN_TASK_PRIORITY      (5U)

/* ---------- 按键 ---------- */

static void app_button_notify(btn_id_e id, const char *name, btn_permission_e permission, btn_event_e event)
{
    (void)permission;
    LOG_INFO("key %s (%s): %s", button_id_to_str(id), (name != NULL) ? name : "?", button_event_to_str(event));

    if (event != BTN_EVENT_SINGLE_CLICK) {
        return;
    }

    if (id == BTN_ID_GPIO0) {
        led_scene_run(LED_SCENE_ID_TRIGGER);
    } else if (id == BTN_ID_GPIO3) {
        led_scene_run(LED_SCENE_ID_SUCCESS);
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

/* ---------- 分阶段初始化（便于单测与换板时替换某一阶段） ---------- */

static status_t app_init_platform(void)
{
    if (log_init(NULL) != STATUS_OK) {
        return STATUS_FAIL;
    }

    if (BoardInit() != STATUS_OK) {
        LOG_ERROR("BoardInit failed");
        return STATUS_FAIL;
    }

    return STATUS_OK;
}

static status_t app_init_button_io(void)
{
    button_init(app_button_notify);

    if (xTaskCreate(button_scan_task, "btn_scan", BTN_SCAN_TASK_STACK_WORDS, NULL,
                    BTN_SCAN_TASK_PRIORITY, NULL) != pdPASS) {
        LOG_ERROR("create btn_scan task failed");
        return STATUS_FAIL;
    }

    LOG_INFO("buttons GPIO0/GPIO3, scan %d Hz; GPIO0 单击=trigger 场景, GPIO3 单击=success",
             FLEX_BTN_SCAN_FREQ_HZ);

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

    /* 上电场景：红灯常亮 2s（与 STM32 bootup 表一致） */
    led_scene_run(LED_SCENE_ID_BOOTUP);

    return STATUS_OK;
}

static status_t app_init(void)
{
    status_t err = app_init_platform();
    if (err != STATUS_OK) {
        return err;
    }

    err = app_init_button_io();
    if (err != STATUS_OK) {
        return err;
    }

    return app_init_led_ui();
}

/* ---------- 主线程 idle（其它工作已放在 FreeRTOS 任务中） ---------- */

static void app_run(void)
{
    const TickType_t period = pdMS_TO_TICKS(APP_LIFECYCLE_IDLE_DELAY_MS);

    for (;;) {
        vTaskDelay(period);
    }
}

/* 静态生命周期表：ROM 常量，避免 app_main 栈上临时结构体歧义 */
static const app_lifecycle_t s_app_lifecycle = {
    .init = app_init,
    .run = app_run,
};

void app_main(void)
{
    status_t err = app_start(&s_app_lifecycle);

    if (err != STATUS_OK) {
        /* log_init 失败时 LOG_* 会静默丢弃，不影响安全停机 */
        LOG_ERROR("application startup failed: %s", status_to_str(err));
    }
}
