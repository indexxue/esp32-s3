/**
 * @file start.c
 * @brief ballot_guard 平台壳层：log、NVS、板级、按键、灯效初始化（掩码见 device_profile）。
 */

#include "start.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "type.h"

#include "device_profile.h"
#include "log.h"
#include "nvs.h"
#include "board.h"
#include "button.h"
#include "flexible_button.h"
#include "led_scene.h"
#include "boot_slot.h"
#include "vote_menu_demo.h"

#include "esp_ota_ops.h"

#if CONFIG_WEB_CTRL_AUTO_START
#include "esp_err.h"
#include "web_ctrl.h"
#include "web_pages.h"

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
    }
    vTaskDelete(NULL);
}
#endif

#define BUTTON_SCAN_PERIOD_MS (1000 / FLEX_BTN_SCAN_FREQ_HZ)
#define BTN_SCAN_TASK_STACK_WORDS (3072U)
#define BTN_SCAN_TASK_PRIORITY (5U)

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

static void app_button_switch_to_other_slot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    status_t                 st;

    if (run == NULL) {
        LOG_ERROR("boot slot: no running partition");
        return;
    }

    if (run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        st = boot_slot_request_factory();
    } else if (run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        st = boot_slot_request_app_a();
    } else {
        LOG_WARN("boot slot: unknown running subtype %u", (unsigned)run->subtype);
        return;
    }

    if (st != ESP_OK) {
        LOG_ERROR("boot slot: esp_ota_set_boot_partition failed: %d", (int)st);
        return;
    }

    LOG_INFO("boot slot: next boot -> other slot, reset");
    boot_slot_system_reset();
}

static void app_button_notify(btn_id_e id, const char *name, btn_permission_e permission, btn_event_e event)
{
    (void)permission;
    LOG_INFO("key %s (%s): %s", button_id_to_str(id), (name != NULL) ? name : "?", button_event_to_str(event));

    if (vote_menu_demo_is_active()) {
        if (vote_menu_demo_on_button(id, event)) {
            return;
        }
    }

    if ((id == BTN_ID_LEFT) && (event == BTN_EVENT_LONG_PRESS)) {
        app_button_switch_to_other_slot();
        return;
    }

    if (event != BTN_EVENT_SINGLE_CLICK) {
        return;
    }

    if (id == BTN_ID_LEFT) {
        led_scene_run(LED_SCENE_ID_TRIGGER);
    } else if (id == BTN_ID_RIGHT) {
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

    LOG_INFO("buttons 左/右 GPIO0/GPIO3, scan %d Hz; 左 单击=灯效 trigger, 左 长按=切换下次启动槽并复位, 右 单击=灯效 success",
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

    led_scene_run(LED_SCENE_ID_BOOTUP);

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

    if (device_profile_board_wants(DEVICE_BOARD_MASK_LCD)) {
        err = vote_menu_demo_start();
        if (err != STATUS_OK) {
            LOG_WARN("vote_menu_demo_start failed");
        }
    }

    LOG_INFO("%s ready (platform_mask=0x%02lX)", product->name,
             (unsigned long)device_profile_platform_mask());
    return STATUS_OK;
}

static void app_run(void)
{
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
