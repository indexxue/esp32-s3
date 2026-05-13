/*
 * @Author: indexxue 2308039918@qq.com
 * @Date: 2026-04-30 21:58:22
 * @LastEditors: indexxue 2308039918@qq.com
 * @LastEditTime: 2026-05-02 10:16:34
 * @FilePath: \ESP32-S3\project\main\main.c
 * @Description: 应用入口：初始化后处理按键/LED 与 QMI+LCD 展示逻辑；外设注册见 board.c。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

#include "type.h"

#include "log.h"
#include "board.h"
#include "sdcard.h"
#include "button.h"
#include "flexible_button.h"
#include "led_scene.h"
#include "qmi8658a.h"
#include "lcd.h"
#include "attitude.h"
#include "start.h"

/* ---------- 本文件内可调参数 ---------- */

#define BUTTON_SCAN_PERIOD_MS (1000 / FLEX_BTN_SCAN_FREQ_HZ)

#define BTN_SCAN_TASK_STACK_WORDS (3072U)
#define BTN_SCAN_TASK_PRIORITY (5U)

/** I2C2 上 QMI8658A 姿态打印周期（毫秒）。 */
#define APP_QMI_ANGLE_LOG_PERIOD_MS (500U)

/** 全屏刷新基准：交替色填充次数（越大计时越稳，启动略慢）。 */
#ifndef APP_LCD_REFRESH_BENCH_FRAMES
#define APP_LCD_REFRESH_BENCH_FRAMES (40)
#endif

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

/** 屏幕底部刷新倾斜角文字的区域（与板级 smoke test 前两行错开）。 */
#define APP_LCD_TILT_LINE_Y (100U)

/** ST7789 全屏刷新 FPS：先逐行 `lcd_fill`，再大事务 `lcd_fill_fast`（SPI DMA + 少次传输）。 */
static void app_lcd_run_dma_refresh_benchmark(void)
{
    st7789_t *lcd = BoardSt7789();
    uint16_t w;
    uint16_t h;
    int64_t t0;
    int64_t dt_us;
    int n;
    double sec;
    double fps_row;
    double fps_bulk;
    uint32_t px;
    double mb_s;

    if (!st7789_is_initialized(lcd)) {
        return;
    }

    w = st7789_display_width(lcd);
    h = st7789_display_height(lcd);
    px = (uint32_t)w * (uint32_t)h;
    n = (int)APP_LCD_REFRESH_BENCH_FRAMES;

    t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        lcd_fill(lcd, 0U, 0U, w, h, (uint16_t)((i & 1) != 0 ? LCD_COLOR_RED : LCD_COLOR_BLUE));
    }
    dt_us = esp_timer_get_time() - t0;
    sec = (dt_us > 0) ? ((double)dt_us / 1000000.0) : 1.0;
    fps_row = (double)n / sec;

    t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        lcd_fill_fast(lcd, 0U, 0U, w, h, (uint16_t)((i & 1) != 0 ? LCD_COLOR_GREEN : LCD_COLOR_MAGENTA));
    }
    dt_us = esp_timer_get_time() - t0;
    sec = (dt_us > 0) ? ((double)dt_us / 1000000.0) : 1.0;
    fps_bulk = (double)n / sec;
    mb_s = ((double)px * 2.0 * fps_bulk) / (1024.0 * 1024.0);

    LOG_INFO(
        "ST7789 DMA bench: %d full frames | row lcd_fill %.2f FPS | bulk lcd_fill_fast %.2f FPS (~%.2f MiB/s pixel "
        "stream) | SPI %u Hz",
        n,
        fps_row,
        fps_bulk,
        mb_s,
        (unsigned int)BOARD_ST7789_SPI_CLOCK_HZ);
}

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

static void app_run(void)
{
    const TickType_t period = pdMS_TO_TICKS(APP_QMI_ANGLE_LOG_PERIOD_MS);
    const float      dt_s   = (float)APP_QMI_ANGLE_LOG_PERIOD_MS / 1000.0f;
    static attitude_complementary_deg_t s_att_deg;
    static bool                         s_att_deg_inited = false;
    qmi8658a_t *imu = BoardQmi8658();

    if (!s_att_deg_inited) {
        /* 与 board 默认 QMI8658A 陀螺档位一致；alpha 越大越信陀螺积分 */
        attitude_complementary_deg_init(&s_att_deg, 0.98f);
        s_att_deg_inited = true;
    }

    app_lcd_run_dma_refresh_benchmark();
    sdcard_mount_smoke_and_benchmark_log();

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
