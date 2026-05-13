/*
 * @Author: indexxue 2308039918@qq.com
 * @Date: 2026-04-30 21:58:22
 * @LastEditors: indexxue 2308039918@qq.com
 * @LastEditTime: 2026-05-02 10:16:34
 * @FilePath: \ESP32-S3\project\main\main.c
 * @Description: 应用入口 `app_main`；业务任务（LCD/SD/IMU 等）见本文件 `application_start_modules_task`。GPIO0 长按切换 OTA 启动槽在 `start.c` 的按键回调中处理。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#include "type.h"

#include "log.h"
#include "start.h"
#include "board.h"
#include "sdcard.h"
#include "qmi8658a.h"
#include "lcd.h"
#include "attitude.h"
#include "button.h"
#include "lcd_gallery.h"

/* ---------- 可调参数（业务任务） ---------- */

/** I2C2 上 QMI8658A 姿态周期（毫秒）。 */
#define APP_QMI_ANGLE_LOG_PERIOD_MS (500U)

/** 屏幕底部刷新倾斜角文字的区域（与板级 smoke test 前两行错开）。 */
#define APP_LCD_TILT_LINE_Y (100U)

#define APP_MODULES_TASK_STACK_WORDS (4096U)
/** 略低于按键扫描任务，避免长 SPI 传输时饿死短周期人机逻辑。 */
#define APP_MODULES_TASK_PRIORITY (4U)

/** 轮询按键周期（毫秒）；用于图库切换，略快于姿态刷新。（GPIO0 长按切槽由 `start.c` 处理，此处仅处理单击。） */
#define APP_GALLERY_BUTTON_POLL_MS (50U)

static void app_lcd_update_tilt_line(float roll_deg, float pitch_deg)
{
    char buf[40];
    uint16_t w;
    st7789_t *lcd = BoardSt7789();

    if (!st7789_is_initialized(lcd)) {
        return;
    }
    w = st7789_display_width(lcd);
    (void)snprintf(buf, sizeof(buf), "R:%.0f P:%.0f deg", (double)roll_deg, (double)pitch_deg);
    lcd_fill(lcd, 0U, APP_LCD_TILT_LINE_Y, w, (uint16_t)(APP_LCD_TILT_LINE_Y + 24U), LCD_COLOR_DARKBLUE);
    lcd_show_string(lcd, 8U, (uint16_t)(APP_LCD_TILT_LINE_Y + 4U), (const uint8_t *)buf, LCD_COLOR_WHITE,
                    LCD_COLOR_DARKBLUE, 16U, 0U);
}

static void application_modules_task(void *arg)
{
    (void)arg;

    const float dt_s = (float)APP_QMI_ANGLE_LOG_PERIOD_MS / 1000.0f;
    static attitude_complementary_deg_t s_att_deg;
    static bool                         s_att_deg_inited = false;
    qmi8658a_t *imu = BoardQmi8658();

    if (!s_att_deg_inited) {
        attitude_complementary_deg_init(&s_att_deg, 0.98f);
        s_att_deg_inited = true;
    }

    st7789_t *lcd = BoardSt7789();

    if (st7789_is_initialized(lcd) && (sdcard_get_card() != NULL)) {
        lcd_gallery_rescan();
        if (lcd_gallery_count() > 0U) {
            (void)lcd_gallery_show_index(lcd, 0U);
        }
    } else if (sdcard_get_card() == NULL) {
        LOG_WARN("SD gallery: card not mounted, skip scan");
    }

    for (;;) {
        int16_t ax = 0;
        int16_t ay = 0;
        int16_t az = 0;
        int16_t gx = 0;
        int16_t gy = 0;
        int16_t gz = 0;
        float roll_acc_deg  = 0.0f;
        float pitch_acc_deg = 0.0f;
        float gx_dps        = 0.0f;
        float gy_dps        = 0.0f;
        btn_id_e    bid;
        btn_event_e bev;
        st7789_t   *lcd = BoardSt7789();
        static uint16_t s_imu_tick;

        button_last_event_get(&bid, &bev);
        if ((bev == BTN_EVENT_SINGLE_CLICK) && (bid == BTN_ID_GPIO0) && st7789_is_initialized(lcd) &&
            (sdcard_get_card() != NULL) && (lcd_gallery_count() > 0U)) {
            button_last_event_clear();
            lcd_gallery_next();
            (void)lcd_gallery_show_index(lcd, lcd_gallery_current());
        }

        s_imu_tick++;
        if (s_imu_tick >= (uint16_t)(APP_QMI_ANGLE_LOG_PERIOD_MS / APP_GALLERY_BUTTON_POLL_MS)) {
            s_imu_tick = 0U;
            if (qmi8658a_read_raw(imu, &ax, &ay, &az, &gx, &gy, &gz) == QMI8658A_OK) {
                (void)gz;
                attitude_tilt_from_accel_raw_deg(ax, ay, az, &roll_acc_deg, &pitch_acc_deg);
                gx_dps = attitude_qmi8658a_gyro_raw_to_dps(gx, (uint8_t)QMI8658A_GYRO_RANGE_2048DPS);
                gy_dps = attitude_qmi8658a_gyro_raw_to_dps(gy, (uint8_t)QMI8658A_GYRO_RANGE_2048DPS);
                attitude_complementary_deg_update(&s_att_deg, dt_s, roll_acc_deg, pitch_acc_deg, gx_dps, gy_dps);
                app_lcd_update_tilt_line(attitude_fused_roll_deg(&s_att_deg), attitude_fused_pitch_deg(&s_att_deg));
            } else {
                LOG_ERROR("QMI8658A read_raw failed");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(APP_GALLERY_BUTTON_POLL_MS));
    }
}

status_t application_start_modules_task(void)
{
    if (xTaskCreate(application_modules_task, "app_mod", APP_MODULES_TASK_STACK_WORDS, NULL, APP_MODULES_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

void app_main(void)
{
    status_t err = app_entry();

    if (err != STATUS_OK) {
        LOG_ERROR("application startup failed: %s", status_to_str(err));
    }
}
