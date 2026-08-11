/**
 * @file camera_app_input.c
 * @brief BTN1 事件与 12V 继电器控制。
 */

#include "camera_app_input.h"

#include <stdbool.h>

#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "button.h"
#include "camera_app_boot.h"
#include "camera_app_config.h"
#include "camera_sensor.h"
#include "device_profile.h"
#include "flexible_button.h"
#include "gpio.h"
#include "led_scene.h"
#include "log.h"
#include "net_wifi.h"
#include "nvs.h"
#include "servo_ctrl.h"

#define BUTTON_SCAN_PERIOD_MS (1000 / FLEX_BTN_SCAN_FREQ_HZ)
#define BTN_SCAN_TASK_STACK_WORDS (3072U)
#define BTN_SCAN_TASK_PRIORITY (5U)
#define WIFI_REPROV_REBOOT_DELAY_MS (500U)
#define WIFI_REPROV_REBOOT_TASK_STACK_WORDS (2048U)
#define WIFI_REPROV_REBOOT_TASK_PRIORITY (5U)

static volatile bool s_wifi_reprov_pending;
static bool          s_pwr_relay_on;

static void wifi_reprov_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_REPROV_REBOOT_DELAY_MS));
    esp_restart();
}

/**
 * 双击 BTN1：清除已存 STA 凭据并重启进入 SoftAP 配网。
 */
static void app_wifi_reprovision_from_button(void)
{
    if (s_wifi_reprov_pending) {
        return;
    }
    s_wifi_reprov_pending = true;

    LOG_INFO("BTN1 double-click → clear STA credentials & SoftAP reprovision");
    camera_app_status_set("wifi: reprovision");
    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED)) {
        led_scene_run(LED_SCENE_ID_PAIRING);
    }

    (void)net_wifi_sta_disconnect();
    if (!nvs_web_ctrl_settings_clear_sta_credentials()) {
        LOG_ERROR("clear STA credentials failed");
        s_wifi_reprov_pending = false;
        camera_app_status_set("wifi: clear fail");
        return;
    }

    camera_sensor_prepare_for_reboot();
    if (xTaskCreate(wifi_reprov_reboot_task, "wifi_rb", WIFI_REPROV_REBOOT_TASK_STACK_WORDS, NULL,
                    WIFI_REPROV_REBOOT_TASK_PRIORITY, NULL) != pdPASS) {
        esp_restart();
    }
}

static status_t app_pwr_relay_set(bool on)
{
    const u32_t level = on ? (u32_t)BOARD_CAMERA_PWR_RELAY_ACTIVE_LEVEL : 0U;

    if (GpioWritePin((s32_t)BOARD_CAMERA_PIN_PWR_RELAY, level) != TRUE) {
        LOG_ERROR("12V relay GPIO%d write failed, esp err %d",
                  BOARD_CAMERA_PIN_PWR_RELAY,
                  (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    s_pwr_relay_on = on;
    LOG_INFO("12V relay %s (GPIO%d=%u)", on ? "ON" : "OFF", BOARD_CAMERA_PIN_PWR_RELAY, (unsigned)level);
    camera_app_status_set(on ? "12V: on" : "12V: off");
    return STATUS_OK;
}

status_t camera_app_pwr_relay_init(void)
{
    GpioPinConfig_t cfg = {0};

    if (GpioDriverInit() != TRUE) {
        LOG_ERROR("12V relay: GpioDriverInit failed, esp err %d", (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    cfg.pin        = (s32_t)BOARD_CAMERA_PIN_PWR_RELAY;
    cfg.mode       = GPIO_MODE_OUTPUT_E;
    cfg.pullUpEn   = GPIO_PULL_DISABLE_E;
    cfg.pullDownEn = GPIO_PULL_DISABLE_E;
    cfg.intrType   = GPIO_INTR_DISABLE_E;

    if (GpioConfigurePin(&cfg) != TRUE) {
        LOG_ERROR("12V relay: configure GPIO%d failed, esp err %d",
                  BOARD_CAMERA_PIN_PWR_RELAY,
                  (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    if (app_pwr_relay_set(false) != STATUS_OK) {
        return STATUS_FAIL;
    }

    LOG_INFO("12V relay ready on GPIO%d (BTN1 single-click toggle)", BOARD_CAMERA_PIN_PWR_RELAY);
    return STATUS_OK;
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
#if CAMERA_APP_CALIB_MODE
        {
            nvs_servo_calib_t cal;
            status_t          st;
            servo_ch_t        ch = SERVO_CH_PAN;

            LOG_INFO("button long press → calib center pose");
            if (nvs_servo_calib_get(&cal)) {
                ch = (cal.channel == NVS_SERVO_CALIB_CH_TILT) ? SERVO_CH_TILT : SERVO_CH_PAN;
                if ((cal.valid_mask & NVS_SERVO_CALIB_VALID_CENTER) != 0U) {
                    st = servo_apply_pose(ch, cal.center.angle_deg, cal.center.offset_deg, cal.center.pulse_us);
                } else {
                    st = servo_set_angle(ch, (float)BOARD_SERVO_CENTER_DEG);
                }
            } else {
                st = servo_set_angle(ch, (float)BOARD_SERVO_CENTER_DEG);
            }
            if (st != STATUS_OK) {
                LOG_WARN("calib center pose failed");
            } else {
                camera_app_status_set("calib: center");
            }
        }
#else
        LOG_INFO("button long press → servo center");
        if (servo_center_all() != STATUS_OK) {
            LOG_WARN("servo_center_all failed");
        } else {
            camera_app_status_set("servo: center");
        }
#endif
        return;
    }

    if ((event == BTN_EVENT_SINGLE_CLICK) && (id == BTN_ID_CONFIRM)) {
        if (app_pwr_relay_set(!s_pwr_relay_on) != STATUS_OK) {
            LOG_WARN("12V relay toggle failed");
        }
        if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED)) {
            led_scene_run(LED_SCENE_ID_TRIGGER);
        }
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

status_t camera_app_button_init(void)
{
    button_init(app_button_notify);

    if (xTaskCreate(button_scan_task, "btn_scan", BTN_SCAN_TASK_STACK_WORDS, NULL, BTN_SCAN_TASK_PRIORITY, NULL) !=
        pdPASS) {
        LOG_ERROR("create btn_scan task failed");
        return STATUS_FAIL;
    }

#if CAMERA_APP_CALIB_MODE
    LOG_INFO("camera: BTN1(GPIO%d) single=12V toggle, long=calib center, double=WiFi SoftAP, scan %d Hz",
             BOARD_CAMERA_PIN_BUTTON,
             FLEX_BTN_SCAN_FREQ_HZ);
#else
    LOG_INFO("camera: BTN1(GPIO%d) single=12V toggle, long=servo center, double=WiFi SoftAP, scan %d Hz",
             BOARD_CAMERA_PIN_BUTTON,
             FLEX_BTN_SCAN_FREQ_HZ);
#endif
    return STATUS_OK;
}
