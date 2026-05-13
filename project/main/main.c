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

#include <math.h>
#include <stdio.h>

#include "type.h"

#include "gpio.h"
#include "i2c.h"
#include "log.h"
#include "board.h"
#include "button.h"
#include "flexible_button.h"
#include "led_scene.h"
#include "qmi8658a.h"
#include "spi.h"
#include "st7789.h"
#include "lcd.h"
#include "start.h"

/* ---------- 本文件内可调参数 ---------- */

#define BUTTON_SCAN_PERIOD_MS (1000 / FLEX_BTN_SCAN_FREQ_HZ)

#define BTN_SCAN_TASK_STACK_WORDS (3072U)
#define BTN_SCAN_TASK_PRIORITY (5U)

/** I2C2 上 QMI8658A 姿态打印周期（毫秒）。 */
#define APP_QMI_ANGLE_LOG_PERIOD_MS (500U)

/** ST7789：SPI2 与 TFT 控制脚（与硬件接线一致）。 */
#define APP_ST7789_SPI_HOST        (SPI_HOST_2_E)
#define APP_ST7789_PIN_SCK         (12)
#define APP_ST7789_PIN_MOSI        (11)
#define APP_ST7789_PIN_CS          (14)
#define APP_ST7789_PIN_DC          (10)
#define APP_ST7789_PIN_RST         (9)
#define APP_ST7789_PIN_BL          (46)
/** `SpiTransmit` 槽位键：CS 由软件控制，SPI 设备使用 `GPIO_NUM_NC`。 */
#define APP_ST7789_SPI_DEV_CS_PIN  (-1)
#define APP_ST7789_SPI_MAX_TX      (32768)
#define APP_ST7789_SPI_CLOCK_HZ    (40000000U)

#ifndef APP_RAD_TO_DEG_F
#define APP_RAD_TO_DEG_F (180.0f / 3.14159265f)
#endif

/* ---------- QMI8658A（I2C2） ---------- */

static qmi8658a_t s_qmi8658;

static void app_qmi_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static int app_qmi_i2c_write(uint8_t addr7, const uint8_t *data, uint16_t len)
{
    if (I2cWrite((s32_t)BOARD_I2C_QMI8658A_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int app_qmi_i2c_write_read(uint8_t addr7,
                                  const uint8_t *write_data,
                                  uint16_t write_len,
                                  uint8_t *read_data,
                                  uint16_t read_len)
{
    if (I2cWriteRead((s32_t)BOARD_I2C_QMI8658A_PORT,
                     (u16_t)addr7,
                     write_data,
                     (usize_t)write_len,
                     read_data,
                     (usize_t)read_len) != TRUE) {
        return -1;
    }
    return 0;
}

/**
 * @brief 由加速度计推算倾斜角（度）：Roll 绕 X，Pitch 绕 Y；无磁力计时无航向角。
 *        静止或低速时较有意义，大幅运动时受线加速度干扰。
 */
static void app_qmi_accel_to_tilt_deg(int16_t ax, int16_t ay, int16_t az, float *roll_deg, float *pitch_deg)
{
    const float fx = (float)ax;
    const float fy = (float)ay;
    const float fz = (float)az;

    *roll_deg  = atan2f(fy, fz) * APP_RAD_TO_DEG_F;
    *pitch_deg = atan2f(-fx, sqrtf(fy * fy + fz * fz)) * APP_RAD_TO_DEG_F;
}

static status_t app_qmi8658_init(void)
{
    qmi8658a_config_t cfg = {0};

    cfg.write       = app_qmi_i2c_write;
    cfg.read        = NULL;
    cfg.write_read  = app_qmi_i2c_write_read;
    cfg.delay_ms    = app_qmi_delay_ms;
    cfg.address     = (uint8_t)BOARD_I2C_QMI8658A_ADDR;
    cfg.accel_range = QMI8658A_ACCEL_RANGE_2G;
    cfg.gyro_range  = QMI8658A_GYRO_RANGE_2048DPS;

    switch (qmi8658a_init_with_config(&s_qmi8658, &cfg)) {
    case QMI8658A_OK:
        LOG_INFO("QMI8658A on I2C2 (port %d) init OK", BOARD_I2C_QMI8658A_PORT);
        return STATUS_OK;
    case QMI8658A_ERROR_ID:
        LOG_ERROR("QMI8658A WHO_AM_I mismatch (check wiring / address 0x%02X)",
                  (unsigned int)BOARD_I2C_QMI8658A_ADDR);
        return STATUS_FAIL;
    default:
        LOG_ERROR("QMI8658A init failed (I2C?)");
        return STATUS_FAIL;
    }
}

/* ---------- ST7789 + SPI2 + lcd 验证 ---------- */

static st7789_t s_st7789;

static void app_st7789_spi_tx(const uint8_t *data, uint16_t len)
{
    (void)SpiTransmit((s32_t)APP_ST7789_SPI_DEV_CS_PIN, data, (usize_t)len);
}

static void app_st7789_pin_cs(int high)
{
    (void)GpioWritePin((s32_t)APP_ST7789_PIN_CS, (u32_t)(high ? 1 : 0));
}

static void app_st7789_pin_dc(int high)
{
    (void)GpioWritePin((s32_t)APP_ST7789_PIN_DC, (u32_t)(high ? 1 : 0));
}

static void app_st7789_pin_rst(int high)
{
    (void)GpioWritePin((s32_t)APP_ST7789_PIN_RST, (u32_t)(high ? 1 : 0));
}

static void app_st7789_pin_bl(int high)
{
    (void)GpioWritePin((s32_t)APP_ST7789_PIN_BL, (u32_t)(high ? 1 : 0));
}

static void app_st7789_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool_t app_st7789_gpio_output_pin(s32_t pin)
{
    GpioPinConfig_t cfg = {0};

    cfg.pin        = pin;
    cfg.mode       = GPIO_MODE_OUTPUT_E;
    cfg.pullUpEn   = GPIO_PULL_DISABLE_E;
    cfg.pullDownEn = GPIO_PULL_DISABLE_E;
    cfg.intrType   = GPIO_INTR_DISABLE_E;

    return GpioConfigurePin(&cfg);
}

/** 清屏 + 边框 + 两行 ASCII，用于确认 SPI / 565 / 字库与方向配置正常。 */
static void app_lcd_smoke_test(void)
{
    uint16_t w = st7789_display_width(&s_st7789);
    uint16_t h = st7789_display_height(&s_st7789);

    lcd_fill(&s_st7789, 0U, 0U, w, h, LCD_COLOR_DARKBLUE);
    lcd_draw_rectangle(&s_st7789, 0U, 0U, (uint16_t)(w - 1U), (uint16_t)(h - 1U), LCD_COLOR_YELLOW);
    lcd_show_string(&s_st7789, 8U, 16U, (const uint8_t *)"ST7789 + lcd OK", LCD_COLOR_WHITE, LCD_COLOR_DARKBLUE, 16U, 0U);
    lcd_show_string(&s_st7789, 8U, 40U, (const uint8_t *)"ESP32-S3 smoke test", LCD_COLOR_CYAN, LCD_COLOR_DARKBLUE, 16U, 0U);
    LOG_INFO("LCD smoke: filled %ux%u, border + 2 lines (16px font)", (unsigned int)w, (unsigned int)h);
}

static status_t app_st7789_init(void)
{
    SpiDriverConfig_t busCfg = {0};
    SpiDeviceConfig_t devCfg = {0};
    st7789_config_t tftCfg  = {0};

    if (GpioDriverInit() != TRUE) {
        LOG_ERROR("ST7789: GpioDriverInit failed, esp err %d", (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    if (app_st7789_gpio_output_pin((s32_t)APP_ST7789_PIN_CS) != TRUE) {
        LOG_ERROR("ST7789: GpioConfigurePin CS GPIO%d failed", APP_ST7789_PIN_CS);
        return STATUS_FAIL;
    }
    if (app_st7789_gpio_output_pin((s32_t)APP_ST7789_PIN_DC) != TRUE) {
        LOG_ERROR("ST7789: GpioConfigurePin DC GPIO%d failed", APP_ST7789_PIN_DC);
        return STATUS_FAIL;
    }
    if (app_st7789_gpio_output_pin((s32_t)APP_ST7789_PIN_RST) != TRUE) {
        LOG_ERROR("ST7789: GpioConfigurePin RST GPIO%d failed", APP_ST7789_PIN_RST);
        return STATUS_FAIL;
    }
    if (app_st7789_gpio_output_pin((s32_t)APP_ST7789_PIN_BL) != TRUE) {
        LOG_ERROR("ST7789: GpioConfigurePin BL GPIO%d failed", APP_ST7789_PIN_BL);
        return STATUS_FAIL;
    }

    busCfg.host            = APP_ST7789_SPI_HOST;
    busCfg.sclkPin         = (s32_t)APP_ST7789_PIN_SCK;
    busCfg.mosiPin         = (s32_t)APP_ST7789_PIN_MOSI;
    busCfg.misoPin         = -1;
    busCfg.quadWpPin       = -1;
    busCfg.quadHdPin       = -1;
    busCfg.maxTransferSize = (s32_t)APP_ST7789_SPI_MAX_TX;
    busCfg.dmaChannel      = (s32_t)3;
    busCfg.intrFlags       = 0;
    busCfg.maxDeviceCount  = 2U;

    if (SpiDriverInit(&busCfg) != TRUE) {
        LOG_ERROR("ST7789: SpiDriverInit failed, esp err %d", (int)SpiGetLastError());
        return STATUS_FAIL;
    }

    devCfg.chipSelectPin  = (s32_t)APP_ST7789_SPI_DEV_CS_PIN;
    devCfg.clockSpeedHz   = APP_ST7789_SPI_CLOCK_HZ;
    devCfg.mode           = SPI_CLOCK_MODE_0_E;
    devCfg.flags          = 0U;
    devCfg.queueSize      = 7U;

    if (SpiRegisterDevice(&devCfg) != TRUE) {
        LOG_ERROR("ST7789: SpiRegisterDevice failed, esp err %d", (int)SpiGetLastError());
        return STATUS_FAIL;
    }

    tftCfg.spi_tx   = app_st7789_spi_tx;
    tftCfg.set_cs   = app_st7789_pin_cs;
    tftCfg.set_dc   = app_st7789_pin_dc;
    tftCfg.set_rst  = app_st7789_pin_rst;
    tftCfg.set_bl   = app_st7789_pin_bl;
    tftCfg.delay_ms = app_st7789_delay_ms;
    tftCfg.rotation = (uint8_t)ST7789_ROT_LANDSCAPE;

    switch (st7789_register(&s_st7789, &tftCfg)) {
    case ST7789_OK:
        LOG_INFO("ST7789 SPI2 SCK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d rot=LANDSCAPE init OK",
                 APP_ST7789_PIN_SCK,
                 APP_ST7789_PIN_MOSI,
                 APP_ST7789_PIN_CS,
                 APP_ST7789_PIN_DC,
                 APP_ST7789_PIN_RST,
                 APP_ST7789_PIN_BL);
        app_lcd_smoke_test();
        return STATUS_OK;
    case ST7789_ERROR_PARAM:
        LOG_ERROR("ST7789 register failed: bad param");
        return STATUS_FAIL;
    default:
        LOG_ERROR("ST7789 register failed");
        return STATUS_FAIL;
    }
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

    if (app_st7789_init() != STATUS_OK) {
        return STATUS_FAIL;
    }

    if (app_qmi8658_init() != STATUS_OK) {
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

/** 屏幕底部刷新倾斜角文字的区域（与 `app_lcd_smoke_test` 前两行错开）。 */
#define APP_LCD_TILT_LINE_Y (100U)

static void app_lcd_update_tilt_line(float roll_deg, float pitch_deg)
{
    char buf[40];
    uint16_t w;

    if (!st7789_is_initialized(&s_st7789)) {
        return;
    }
    w = st7789_display_width(&s_st7789);
    (void)snprintf(buf, sizeof(buf), "R:%.0f P:%.0f deg", (double)roll_deg, (double)pitch_deg);
    lcd_fill(&s_st7789, 0U, APP_LCD_TILT_LINE_Y, w, (uint16_t)(APP_LCD_TILT_LINE_Y + 24U), LCD_COLOR_DARKBLUE);
    lcd_show_string(&s_st7789, 8U, (uint16_t)(APP_LCD_TILT_LINE_Y + 4U), (const uint8_t *)buf, LCD_COLOR_WHITE,
                    LCD_COLOR_DARKBLUE, 16U, 0U);
}

static void app_run(void)
{
    const TickType_t period = pdMS_TO_TICKS(APP_QMI_ANGLE_LOG_PERIOD_MS);

    for (;;) {
        int16_t ax = 0;
        int16_t ay = 0;
        int16_t az = 0;
        int16_t gx = 0;
        int16_t gy = 0;
        int16_t gz = 0;
        float roll_deg = 0.0f;
        float pitch_deg = 0.0f;

        if (qmi8658a_read_raw(&s_qmi8658, &ax, &ay, &az, &gx, &gy, &gz) == QMI8658A_OK) {
            (void)gx;
            (void)gy;
            (void)gz;
            app_qmi_accel_to_tilt_deg(ax, ay, az, &roll_deg, &pitch_deg);
            LOG_INFO("I2C2 QMI8658A tilt: Roll=%.1f deg, Pitch=%.1f deg | accel raw ax=%d ay=%d az=%d",
                     roll_deg,
                     pitch_deg,
                     (int)ax,
                     (int)ay,
                     (int)az);
            app_lcd_update_tilt_line(roll_deg, pitch_deg);
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
