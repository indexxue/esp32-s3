/**
 * @file camera_app_boot.c
 * @brief 板级初始化、摄像头模块任务、WiFi/Web 门控。
 */

#include "camera_app_boot.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board.h"
#include "camera_app_config.h"
#include "camera_sensor.h"
#include "camera_spi_host.h"
#include "device_profile.h"
#include "led_scene.h"
#include "log.h"
#include "net_wifi.h"
#include "nvs.h"
#include "ota.h"
#include "servo_ctrl.h"
#include "web_pages.h"

#if CAMERA_APP_DETECT_MODE
#include "camera_model.h"
#include "servo_ball_follow.h"
#endif

#if CAMERA_APP_CALIB_MODE
#include "cmd.h"
#include "servo_calib_cmd.h"
#endif

#if CAMERA_ENABLE_LCD
#include "camera_ui.h"
#endif

#if CONFIG_WEB_CTRL_AUTO_START
#include "esp_err.h"
#include "web_ctrl.h"
#include "web_server.h"
#endif

#define CAMERA_WIFI_WAIT_MS (8000U)
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

void camera_app_status_set(const char *line)
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
            LOG_WARN("WiFi start wait timeout");
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

    if (s_cam_boot_done != NULL) {
        if (xSemaphoreTake(s_cam_boot_done, pdMS_TO_TICKS(CAMERA_WIFI_WAIT_MS)) != pdTRUE) {
            LOG_WARN("camera boot wait timeout, start WiFi anyway");
        }
    }

    web_ctrl_config_init_defaults(&wcfg);
    web_ctrl_config_merge_nvs(&wcfg);
    wcfg.root_get_handler = web_pages_root_get_handler;
#if CAMERA_APP_CALIB_MODE
    web_server_prefer_lru_purge(true);
#endif
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
#if CAMERA_APP_CALIB_MODE
        {
            char ipbuf[20];

            if (net_wifi_format_ipv4_for_display(ipbuf, sizeof(ipbuf))) {
                LOG_INFO("calib web open: http://%s/  (NOT 192.168.4.1 if STA)", ipbuf);
            } else {
                LOG_INFO("calib web: waiting IP; SoftAP fallback http://192.168.4.1/");
            }
        }
#endif
    }
    vTaskDelete(NULL);
}
#endif

static void camera_modules_boot(void)
{
    camera_ui_boot();

    if (camera_sensor_init() != STATUS_OK) {
        camera_app_status_set("cam: init fail");
        LOG_WARN("camera_sensor_init failed");
        camera_signal_wifi_may_start();
        return;
    }

    if (camera_sensor_preview_start() != STATUS_OK) {
        LOG_WARN("camera sensor preview start failed");
        camera_app_status_set("cam: preview fail");
    } else {
        camera_app_status_set("cam: preview");
    }

    /* SoftAP beacon 需内部 DRAM：先放行 WiFi，再按需加载检测模型。 */
    camera_signal_wifi_may_start();
    camera_wait_wifi_started();

#if CAMERA_APP_CALIB_MODE
    LOG_INFO("CAMERA_APP_CALIB_MODE=1: detect skipped (servo calib)");
    camera_app_status_set("calib: servo");
#elif CAMERA_APP_COLLECT_MODE
    LOG_INFO("CAMERA_APP_COLLECT_MODE=1: detect skipped (JPEG collect)");
    camera_app_status_set("collect: jpeg");
#elif CAMERA_APP_DETECT_MODE
    if (camera_model_start() != STATUS_OK) {
        LOG_WARN("camera_model_start failed");
        camera_app_status_set("detect: fail");
    } else {
        camera_app_status_set("detect: ball");
        if (servo_ball_follow_start() != STATUS_OK) {
            LOG_WARN("servo_ball_follow_start skipped (need NVS L/C/R)");
        }
    }
#else
    LOG_INFO("CAMERA_APP_DETECT_MODE=0: preview only (ESPDet off)");
    camera_app_status_set("preview");
#endif
}

static void camera_modules_task(void *arg)
{
    (void)arg;
    camera_modules_boot();
    camera_status_refresh_loop();
}

static status_t camera_start_modules_task(void)
{
    if (xTaskCreate(camera_modules_task, "cam_mod", CAMERA_MODULES_TASK_STACK_WORDS, NULL,
                    CAMERA_MODULES_TASK_PRIORITY, NULL) != pdPASS) {
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

status_t camera_app_boot_init(void)
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
#if CAMERA_APP_CALIB_MODE
    else {
        nvs_servo_calib_t cal;
        float             lo = 0.0f;
        float             hi = (float)BOARD_SERVO_ANGLE_MAX_DEG;
        servo_ch_t        ch = SERVO_CH_PAN;

        if (servo_set_limits(&lo, &hi, &lo, &hi) != STATUS_OK) {
            LOG_WARN("calib: open soft limits failed");
        }
        if (nvs_servo_calib_get(&cal)) {
            ch = (cal.channel == NVS_SERVO_CALIB_CH_TILT) ? SERVO_CH_TILT : SERVO_CH_PAN;
            if ((cal.valid_mask & NVS_SERVO_CALIB_VALID_CENTER) != 0U) {
                if (servo_apply_pose(ch, cal.center.angle_deg, cal.center.offset_deg, cal.center.pulse_us) !=
                    STATUS_OK) {
                    LOG_WARN("calib: apply NVS center pose failed");
                } else {
                    LOG_INFO("calib: ch=%u parked at NVS center", (unsigned)cal.channel);
                }
            }
        }
    }
#endif

#if CAMERA_APP_CALIB_MODE
    LOG_INFO("calib: SPI host skipped");
#else
    if (camera_spi_host_start() != STATUS_OK) {
        LOG_WARN("camera_spi_host_start failed (MCU link unavailable)");
    }
#endif

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

status_t camera_app_led_init(void)
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

status_t camera_app_calib_cmd_init(void)
{
#if CAMERA_APP_CALIB_MODE
    cmd_set_project_register_fn(servo_calib_cmd_register);
    if (cmd_usb_line_service_start() != STATUS_OK) {
        LOG_WARN("cmd_usb_line_service_start failed (use web only)");
    } else {
        LOG_INFO("calib USB cmd ready: type 'help' then 'servo pan 180'");
    }
#endif
    return STATUS_OK;
}
