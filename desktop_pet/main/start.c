/**
 * @file start.c
 * @brief desktop_pet 平台壳层：log / NVS / 板级 / 按键 / LED；外设驱动按 ENABLE 宏接入。
 */

#include "start.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "button.h"
#include "device_profile.h"
#include "agent.h"
#include "audio.h"
#include "ui.h"
#include "esp_system.h"
#include "flexible_button.h"
#include "led_scene.h"
#include "log.h"
#include "net_wifi.h"
#include "nvs.h"
#include "ota.h"

#if CONFIG_WEB_CTRL_AUTO_START && DESKTOP_PET_ENABLE_WIFI_WEB
#include "esp_err.h"
#include "web_ctrl.h"
#include "web_pages.h"
#endif

#define BUTTON_SCAN_PERIOD_MS (1000 / FLEX_BTN_SCAN_FREQ_HZ)
#define BTN_SCAN_TASK_STACK_WORDS (3072U)
#define BTN_SCAN_TASK_PRIORITY (5U)

#if CONFIG_WEB_CTRL_AUTO_START && DESKTOP_PET_ENABLE_WIFI_WEB
#define WEB_CTRL_BOOT_TASK_STACK_WORDS (10240U)
#define WEB_CTRL_BOOT_TASK_PRIORITY (3U)
#endif

#define WIFI_REPROV_REBOOT_DELAY_MS (500U)
#define WIFI_REPROV_REBOOT_TASK_STACK_WORDS (2048U)
#define WIFI_REPROV_REBOOT_TASK_PRIORITY (5U)

static volatile bool s_wifi_reprov_pending;

static void wifi_reprov_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_REPROV_REBOOT_DELAY_MS));
    esp_restart();
}

static void app_wifi_reprovision_from_button(void)
{
    if (s_wifi_reprov_pending) {
        return;
    }
    s_wifi_reprov_pending = true;

    LOG_INFO("BTN double-click: clear STA credentials, SoftAP reprovision");
    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED)) {
        led_scene_run(LED_SCENE_ID_PAIRING);
    }

    (void)net_wifi_sta_disconnect();
    if (!nvs_web_ctrl_settings_clear_sta_credentials()) {
        LOG_ERROR("clear STA credentials failed");
        s_wifi_reprov_pending = false;
        return;
    }

    if (xTaskCreate(wifi_reprov_reboot_task, "wifi_rb", WIFI_REPROV_REBOOT_TASK_STACK_WORDS, NULL,
                    WIFI_REPROV_REBOOT_TASK_PRIORITY, NULL) != pdPASS) {
        esp_restart();
    }
}

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

static void app_ota_confirm_running_image(void)
{
    const status_t ota_st = ota_confirm_running_image();

    if (ota_st != STATUS_OK) {
        LOG_WARN("ota_confirm_running_image: %s", status_to_str(ota_st));
    }
}

static void app_button_notify(btn_id_e id, const char *name, btn_permission_e permission, btn_event_e event)
{
    (void)name;
    (void)permission;

    if (id != BTN_ID_CONFIRM) {
        return;
    }

    switch (event) {
    case BTN_EVENT_SINGLE_CLICK:
#if DESKTOP_PET_ENABLE_DEBUG_UI
        LOG_INFO("BTN single-click: toggle debug overlay");
#else
        LOG_INFO("BTN single-click: reserved (debug UI off)");
#endif
        desktop_pet_ui_toggle_debug();
        break;
    case BTN_EVENT_DOUBLE_CLICK:
        app_wifi_reprovision_from_button();
        break;
    default:
        break;
    }
}

static void btn_scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        button_schedule();
        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_PERIOD_MS));
    }
}

static status_t app_init_button_io(void)
{
    button_init(app_button_notify);

    if (xTaskCreate(btn_scan_task, "btn_scan", BTN_SCAN_TASK_STACK_WORDS, NULL, BTN_SCAN_TASK_PRIORITY,
                    NULL) != pdPASS) {
        LOG_ERROR("btn_scan task create failed");
        return STATUS_FAIL;
    }

    LOG_INFO("button ready GPIO%d", BOARD_DESKTOP_PET_PIN_BUTTON);
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
    LOG_INFO("WS2812 on GPIO%d x%u", BOARD_DESKTOP_PET_WS2812_PIN,
             (unsigned)BOARD_DESKTOP_PET_WS2812_COUNT);
    return STATUS_OK;
}

#if CONFIG_WEB_CTRL_AUTO_START && DESKTOP_PET_ENABLE_WIFI_WEB
static void web_ctrl_boot_task(void *arg)
{
    web_ctrl_config_t wcfg;

    (void)arg;
    web_ctrl_config_init_defaults(&wcfg);
    web_ctrl_config_merge_nvs(&wcfg);
    wcfg.root_get_handler      = web_pages_root_get_handler;
    wcfg.gallery_http_register = web_pages_sd_http_register;
    {
        const esp_err_t werr = web_ctrl_start(&wcfg);

        if (werr != ESP_OK) {
            LOG_WARN("web_ctrl_start failed: %s", esp_err_to_name(werr));
        } else {
            app_ota_confirm_running_image();
            LOG_INFO("web_ctrl started (SD file manager)");
        }
    }
    vTaskDelete(NULL);
}
#endif

static status_t app_init(void)
{
    status_t err;
    const device_product_profile_t *product;

    if (log_init(NULL) != STATUS_OK) {
        return STATUS_FAIL;
    }

    nvs_init();

    if (BoardInit() != STATUS_OK) {
        LOG_ERROR("BoardInit failed");
        return STATUS_FAIL;
    }

    product = device_profile_product();

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

#if CONFIG_WEB_CTRL_AUTO_START && DESKTOP_PET_ENABLE_WIFI_WEB
    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_WEB)) {
        if (xTaskCreate(web_ctrl_boot_task, "web_boot", WEB_CTRL_BOOT_TASK_STACK_WORDS, NULL,
                        WEB_CTRL_BOOT_TASK_PRIORITY, NULL) != pdPASS) {
            LOG_WARN("web_boot task create failed");
        }
    } else {
        app_ota_confirm_running_image();
    }
#else
    app_ota_confirm_running_image();
#endif

#if DESKTOP_PET_ENABLE_AUDIO
    if (desktop_pet_audio_init() != STATUS_OK) {
        LOG_WARN("desktop_pet_audio_init failed");
    }
#endif

#if DESKTOP_PET_ENABLE_LCD
    if (desktop_pet_ui_start() != STATUS_OK) {
        LOG_WARN("desktop_pet_ui_start failed");
    }
#endif

    if (desktop_pet_agent_init() != STATUS_OK) {
        LOG_WARN("desktop_pet_agent_init failed");
    }

    LOG_INFO("%s ready lcd=%d touch=%d imu=%d audio=%d motor=%d sd=%d (platform_mask=0x%02lX)",
             product->name,
             DESKTOP_PET_ENABLE_LCD,
             DESKTOP_PET_ENABLE_TOUCH,
             DESKTOP_PET_ENABLE_IMU,
             DESKTOP_PET_ENABLE_AUDIO,
             DESKTOP_PET_ENABLE_MOTOR,
             DESKTOP_PET_ENABLE_SDCARD,
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
