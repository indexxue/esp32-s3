/**
 * @file start.c
 * @brief 生命周期调度（init → run）与平台壳层：log、板级、按键、灯效；业务任务由 `main.c` 拉起。
 */

#include "start.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "type.h"

#include "log.h"
#include "persist.h"
#include "board.h"
#include "button.h"
#include "flexible_button.h"
#include "led_scene.h"
#include "cmd.h"

/* ---------- 可调参数 ---------- */

#define BUTTON_SCAN_PERIOD_MS (1000 / FLEX_BTN_SCAN_FREQ_HZ)

#define BTN_SCAN_TASK_STACK_WORDS (3072U)
#define BTN_SCAN_TASK_PRIORITY (5U)

/* ---------- 默认 idle（run 为 NULL 或壳层常驻时） ---------- */

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
    /* 不可达，满足部分静态分析工具 */
    return STATUS_OK;
}

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

/* ---------- 分阶段初始化 ---------- */

static status_t app_init_platform(void)
{
    if (log_init(NULL) != STATUS_OK) {
        return STATUS_FAIL;
    }

    /* NVS：依赖 log，便于打印初始化与 boot 摘要（见 persist.c 中 nvs_init / nvs_print_boot_info） */
    nvs_init();

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

    LOG_INFO("buttons GPIO0/GPIO3, scan %d Hz; GPIO0 单击=灯效 trigger + SD 图库下一张, GPIO3 单击=灯效 success",
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

    err = app_init_led_ui();
    if (err != STATUS_OK) {
        return err;
    }

    if (cmd_usb_line_service_start() != STATUS_OK) {
        LOG_WARN("USB factory cmd line not started (check USB Serial/JTAG driver)");
    }

    return STATUS_OK;
}

/**
 * 拉起业务任务后进入低占空 idle，不返回（与原先在 `app_main` 上下文中跑无限循环语义一致）。
 */
static void app_run(void)
{
    if (application_start_modules_task() != STATUS_OK) {
        LOG_ERROR("application_start_modules_task failed");
    }

    app_idle_default();
}

static const app_lifecycle_t s_default_lifecycle = {
    .init = app_init,
    .run = app_run,
};

status_t app_entry(void)
{
    return app_start(&s_default_lifecycle);
}
