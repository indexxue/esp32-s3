/**
 * @file start.c
 * @brief camera 平台壳层：log、NVS、板级、单键、Web/OTA、摄像头预览。
 */

#include "start.h"

#include <stdbool.h>
#include <string.h>

#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "type.h"

#include "board.h"
#include "button.h"
#include "camera_model.h"
#include "camera_sensor.h"
#include "camera_spi_host.h"
#include "device_profile.h"
#include "flexible_button.h"
#include "led_scene.h"
#include "log.h"
#include "net_wifi.h"
#include "nvs.h"
#include "ota.h"
#include "servo_ctrl.h"
#include "web_pages.h"

#if CAMERA_ENABLE_LCD
#include "camera_ui.h"
#endif

#if CONFIG_WEB_CTRL_AUTO_START
#include "esp_err.h"
#include "web_ctrl.h"
#include "web_server.h"
#endif

/** WiFi 等待摄像头 detect/set_format/stream 完成的上限。 */
#define CAMERA_WIFI_WAIT_MS (8000U)

#define BUTTON_SCAN_PERIOD_MS (1000 / FLEX_BTN_SCAN_FREQ_HZ)
#define BTN_SCAN_TASK_STACK_WORDS (3072U)
#define BTN_SCAN_TASK_PRIORITY (5U)
#define WIFI_REPROV_REBOOT_DELAY_MS (500U)
#define WIFI_REPROV_REBOOT_TASK_STACK_WORDS (2048U)
#define WIFI_REPROV_REBOOT_TASK_PRIORITY (5U)

#define CAMERA_MODULES_TASK_STACK_WORDS (4096U)
#define CAMERA_MODULES_TASK_PRIORITY (4U)
#define CAMERA_STATUS_REFRESH_MS (1000U)
#define CAMERA_IP_LOG_INTERVAL_MS (30000U)

#if CONFIG_WEB_CTRL_AUTO_START
#define WEB_CTRL_BOOT_TASK_STACK_WORDS (10240U)
#define WEB_CTRL_BOOT_TASK_PRIORITY (3U)
#endif

static SemaphoreHandle_t s_cam_boot_done;
static bool              s_cam_wifi_gate_opened;
static volatile bool     s_wifi_reprov_pending;

/* -------------------------------------------------------------------------- */
/* LCD 状态栏（CAMERA_ENABLE_LCD=0 时为空操作）                                  */
/* -------------------------------------------------------------------------- */

static void camera_status_set(const char *line)
{
#if CAMERA_ENABLE_LCD
    st7789_t *lcd = BoardSt7789();

    if ((line != NULL) && st7789_is_initialized(lcd)) {
        camera_ui_set_status_line(lcd, line);
    }
#else
    (void)line;
#endif
}

static void camera_ui_boot(void)
{
#if CAMERA_ENABLE_LCD
    st7789_t *lcd = BoardSt7789();

    if (st7789_is_initialized(lcd)) {
        (void)camera_ui_init(lcd);
    }
#endif
}

static void camera_status_refresh_loop(void)
{
    char       ipbuf[20];
    TickType_t last_ip_log = 0;
#if CAMERA_ENABLE_LCD
    st7789_t *lcd = BoardSt7789();
    char      last_line[48];

    last_line[0] = '\0';
#endif

    for (;;) {
        if (!net_wifi_format_ipv4_for_display(ipbuf, sizeof(ipbuf))) {
            (void)strncpy(ipbuf, "---", sizeof(ipbuf) - 1U);
            ipbuf[sizeof(ipbuf) - 1U] = '\0';
        }

        {
            const TickType_t now = xTaskGetTickCount();

            if ((last_ip_log == 0) || ((now - last_ip_log) >= pdMS_TO_TICKS(CAMERA_IP_LOG_INTERVAL_MS))) {
                LOG_INFO("ip: %s", ipbuf);
                last_ip_log = now;
            }
        }

#if CAMERA_ENABLE_LCD
        if (st7789_is_initialized(lcd) && (camera_sensor_is_ready() != FALSE)) {
            if (strncmp(ipbuf, last_line, sizeof(last_line)) != 0) {
                camera_ui_set_status_line(lcd, ipbuf);
                (void)strncpy(last_line, ipbuf, sizeof(last_line) - 1U);
                last_line[sizeof(last_line) - 1U] = '\0';
            }
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(CAMERA_STATUS_REFRESH_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* 生命周期                                                                     */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/* WiFi 门控：SCCB/预览就绪后再启射频                                             */
/* -------------------------------------------------------------------------- */

static void camera_signal_wifi_may_start(void)
{
    if (s_cam_wifi_gate_opened) {
        return;
    }
    s_cam_wifi_gate_opened = true;
    if (s_cam_boot_done != NULL) {
        (void)xSemaphoreGive(s_cam_boot_done);
    }
}

static void camera_wait_wifi_started(void)
{
#if !CONFIG_WEB_CTRL_AUTO_START
    return;
#else
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CAMERA_WIFI_WAIT_MS);

    if (!device_profile_platform_wants(DEVICE_PLATFORM_MASK_WEB)) {
        return;
    }

    while (!net_wifi_is_started()) {
        if (xTaskGetTickCount() >= deadline) {
            LOG_WARN("WiFi start wait timeout, load model anyway");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif
}

#if CONFIG_WEB_CTRL_AUTO_START
static void web_ctrl_boot_task(void *arg)
{
    web_ctrl_config_t wcfg;

    (void)arg;

    /* 等摄像头 SCCB 写完再启 WiFi，避免 set_format 与射频并发（不等 ball 模型）。 */
    if (s_cam_boot_done != NULL) {
        if (xSemaphoreTake(s_cam_boot_done, pdMS_TO_TICKS(CAMERA_WIFI_WAIT_MS)) != pdTRUE) {
            LOG_WARN("camera boot wait timeout, start WiFi anyway");
        }
    }

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

/* -------------------------------------------------------------------------- */
/* 按键：单击 LED / 长按舵机回中 / 双击 SoftAP 重配网                              */
/* -------------------------------------------------------------------------- */

static void wifi_reprov_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_REPROV_REBOOT_DELAY_MS));
    esp_restart();
}

/**
 * 双击 GPIO0：清除已存 STA 凭据并重启进入 SoftAP 配网。
 * 用于 STA 已连上但无法访问网页、或配网信息错误时的本地恢复。
 */
static void app_wifi_reprovision_from_button(void)
{
    if (s_wifi_reprov_pending) {
        return;
    }
    s_wifi_reprov_pending = true;

    LOG_INFO("GPIO0 double-click → clear STA credentials & SoftAP reprovision");
    camera_status_set("wifi: reprovision");
    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED)) {
        led_scene_run(LED_SCENE_ID_PAIRING);
    }

    /* 尽力断开；即使 Wi‑Fi 未起或非 STA，仍清 NVS 后重启。 */
    (void)net_wifi_sta_disconnect();
    if (!nvs_web_ctrl_settings_clear_sta_credentials()) {
        LOG_ERROR("clear STA credentials failed");
        s_wifi_reprov_pending = false;
        camera_status_set("wifi: clear fail");
        return;
    }

    camera_sensor_prepare_for_reboot();
    if (xTaskCreate(wifi_reprov_reboot_task, "wifi_rb", WIFI_REPROV_REBOOT_TASK_STACK_WORDS, NULL,
                    WIFI_REPROV_REBOOT_TASK_PRIORITY, NULL) != pdPASS) {
        esp_restart();
    }
}

static void app_button_notify(btn_id_e id, const char *name, btn_permission_e permission, btn_event_e event)
{
    LOG_INFO("key %s (%s): %s", button_id_to_str(id), (name != NULL) ? name : "?", button_event_to_str(event));

    if (event == BTN_EVENT_DOUBLE_CLICK) {
        if ((permission & BTN_PERMISSION_PAIR) != 0) {
            app_wifi_reprovision_from_button();
        }
        return;
    }

    if (event == BTN_EVENT_LONG_PRESS) {
        LOG_INFO("button long press → servo center");
        if (servo_center_all() != STATUS_OK) {
            LOG_WARN("servo_center_all failed");
        } else {
            camera_status_set("servo: center");
        }
        return;
    }

    if ((event == BTN_EVENT_SINGLE_CLICK) && (id == BTN_ID_CONFIRM)) {
        if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED)) {
            led_scene_run(LED_SCENE_ID_TRIGGER);
        }
        camera_status_set("key: pressed");
    }
}

static void button_scan_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(BUTTON_SCAN_PERIOD_MS);

    (void)arg;
    for (;;) {
        button_schedule();
        vTaskDelay(period);
    }
}

/* -------------------------------------------------------------------------- */
/* 摄像头模块任务：传感器 → WiFi 门控 → 检测模型 → 状态循环                        */
/* -------------------------------------------------------------------------- */

static void camera_modules_boot(void)
{
    camera_ui_boot();

    if (camera_sensor_init() != STATUS_OK) {
        camera_status_set("cam: init fail");
        LOG_WARN("camera_sensor_init failed");
        camera_signal_wifi_may_start();
        return;
    }

    if (camera_sensor_preview_start() != STATUS_OK) {
        LOG_WARN("camera sensor preview start failed");
        camera_status_set("cam: preview fail");
    } else {
        camera_status_set("cam: preview");
    }

    /*
     * SoftAP beacon 必须走内部 DRAM；先放行 WiFi，等其启动后再加载 ball 模型，
     * 避免 alloc eb fail → hostap 空指针崩溃重启环。
     */
    camera_signal_wifi_may_start();
    camera_wait_wifi_started();

    if (camera_model_start() != STATUS_OK) {
        LOG_WARN("camera_model_start failed");
        camera_status_set("detect: fail");
    } else {
        camera_status_set("detect: ball");
    }
}

static void camera_modules_task(void *arg)
{
    (void)arg;
    camera_modules_boot();
    camera_status_refresh_loop();
}

status_t camera_start_modules_task(void)
{
    if (xTaskCreate(camera_modules_task, "cam_mod", CAMERA_MODULES_TASK_STACK_WORDS, NULL,
                    CAMERA_MODULES_TASK_PRIORITY, NULL) != pdPASS) {
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

/* -------------------------------------------------------------------------- */
/* 平台初始化                                                                   */
/* -------------------------------------------------------------------------- */

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

    if (servo_init() != STATUS_OK) {
        LOG_WARN("servo_init failed (gimbal unavailable)");
    }

    if (camera_spi_host_start() != STATUS_OK) {
        LOG_WARN("camera_spi_host_start failed (MCU link unavailable)");
    }

    /*
     * 摄像头 SCCB 长寄存器表写入对总线抖动敏感。
     * 先拉起 camera 任务；WiFi 等 SCCB/预览就绪后再启（不等 ball 模型）。
     */
    s_cam_boot_done = xSemaphoreCreateBinary();
    if (s_cam_boot_done == NULL) {
        LOG_WARN("cam boot semaphore create failed");
    }

    if (camera_start_modules_task() != STATUS_OK) {
        LOG_WARN("camera_start_modules_task failed");
        camera_signal_wifi_may_start();
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

    LOG_INFO("camera: GPIO0 long=servo center, double=WiFi SoftAP reprovision, scan %d Hz",
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
