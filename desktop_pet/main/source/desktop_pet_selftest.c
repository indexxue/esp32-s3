/**
 * @file desktop_pet_selftest.c
 * @brief 桌宠外设烟测（参考程序，默认不参与编译）。见 desktop_pet_selftest.h。
 */

#include "desktop_pet_selftest.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "board.h"
#include "cmd.h"
#include "device_profile.h"
#include "led_scene.h"
#include "log.h"
#include "gpio.h"
#include "i2c.h"
#include "sdcard.h"

#if DESKTOP_PET_ENABLE_MOTOR
#include "pwm.h"
#include "tb6612.h"
#endif

#if DESKTOP_PET_ENABLE_LCD
#include "gc9a01.h"
#endif

#if DESKTOP_PET_ENABLE_IMU
#include "qmi8658a.h"
#endif

#ifndef DESKTOP_PET_ENABLE_SELFTEST
#define DESKTOP_PET_ENABLE_SELFTEST 1
#endif

#ifndef DESKTOP_PET_SELFTEST_BOOT_RUN
#define DESKTOP_PET_SELFTEST_BOOT_RUN 1
#endif

#define SELFTEST_TASK_STACK_WORDS (4096U)
#define SELFTEST_TASK_PRIORITY (3U)
#if DESKTOP_PET_ENABLE_MOTOR
#define MOTOR_PWM_FREQ_HZ (20000U)
#define MOTOR_PWM_DUTY_RES PWM_DUTY_RES_10BIT_E
#define MOTOR_TEST_MS (800U)
/** 左轮前转 + 右轮后转（原地右转）时长。 */
#define MOTOR_SPIN_TEST_MS (5000U)
#define MOTOR_TEST_SPEED_PERMILLE (350U)
#endif

static void selftest_log_result(const char *name, bool ok, const char *detail)
{
    if (ok) {
        LOG_INFO("selftest %-6s OK%s%s", name, (detail != NULL && detail[0] != '\0') ? " " : "",
                 (detail != NULL) ? detail : "");
    } else {
        LOG_WARN("selftest %-6s FAIL%s%s", name, (detail != NULL && detail[0] != '\0') ? " " : "",
                 (detail != NULL) ? detail : "");
    }
}

static bool selftest_i2c(void)
{
    u16_t n;
    char detail[64];

    n = I2cScanBus7Bit((s32_t)BOARD_I2C_BUS1_HW_PORT, NULL, NULL);
    (void)snprintf(detail, sizeof(detail), "port%d found=%u", (int)BOARD_I2C_BUS1_HW_PORT, (unsigned)n);
    selftest_log_result("i2c", n > 0U, detail);
    return n > 0U;
}

static bool selftest_bat(void)
{
    battery_voltage_t v;
    battery_info_t info;
    char detail[80];
    uint32_t mv;

    if (!device_profile_board_wants(DEVICE_BOARD_MASK_BATTERY)) {
        selftest_log_result("bat", false, "mask off");
        return false;
    }

    mv = battery_voltage_read_mv(&v);
    (void)battery_info_read(&info, &v);
    if (mv == 0U) {
        selftest_log_result("bat", false, "adc=0");
        return false;
    }
    (void)snprintf(detail, sizeof(detail), "%lumV pct=%u chg=%u", (unsigned long)mv, (unsigned)info.percent,
                   (unsigned)info.charging);
    selftest_log_result("bat", true, detail);
    return true;
}

static bool selftest_lcd(void)
{
#if !DESKTOP_PET_ENABLE_LCD
    selftest_log_result("lcd", false, "ENABLE_LCD=0");
    return false;
#else
    gc9a01_t *lcd = BoardGc9a01();
    uint16_t w;
    uint16_t h;
    static const uint16_t colors[] = {0xF800U, 0x07E0U, 0x001FU, 0xFFFFU, 0x0000U};
    size_t i;

    if ((lcd == NULL) || !gc9a01_is_initialized(lcd)) {
        selftest_log_result("lcd", false, "not init");
        return false;
    }
    w = gc9a01_display_width(lcd);
    h = gc9a01_display_height(lcd);
    for (i = 0; i < (sizeof(colors) / sizeof(colors[0])); i++) {
        gc9a01_fill(lcd, 0U, 0U, w, h, colors[i]);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    gc9a01_fill(lcd, 0U, 0U, w, h, 0x01CFU);
    selftest_log_result("lcd", true, "color bars");
    return true;
#endif
}

static bool selftest_imu(void)
{
#if !DESKTOP_PET_ENABLE_IMU
    selftest_log_result("imu", false, "ENABLE_IMU=0");
    return false;
#else
    qmi8658a_t *imu = BoardQmi8658();
    int16_t ax, ay, az, gx, gy, gz;
    char detail[96];

    if ((imu == NULL) || !imu->initialized) {
        selftest_log_result("imu", false, "not init");
        return false;
    }
    if (qmi8658a_read_raw(imu, &ax, &ay, &az, &gx, &gy, &gz) != QMI8658A_OK) {
        selftest_log_result("imu", false, "read_raw");
        return false;
    }
    (void)snprintf(detail, sizeof(detail), "a=%d,%d,%d g=%d,%d,%d", (int)ax, (int)ay, (int)az, (int)gx, (int)gy,
                   (int)gz);
    selftest_log_result("imu", true, detail);
    return true;
#endif
}

static bool selftest_led(void)
{
    if (!device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED)) {
        selftest_log_result("led", false, "mask off");
        return false;
    }
    led_scene_run(LED_SCENE_ID_WORKING);
    vTaskDelay(pdMS_TO_TICKS(1200));
    led_scene_run(LED_SCENE_ID_SUCCESS);
    selftest_log_result("led", true, "rainbow->success");
    return true;
}

#if DESKTOP_PET_ENABLE_MOTOR
static tb6612_t s_tb6612;
static bool s_motor_ready;

static void motor_gpio_set(uint8_t pin_id, uint8_t level)
{
    s32_t pin = -1;

    switch (pin_id) {
    case TB6612_PIN_IN1:
        pin = (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_AIN1;
        break;
    case TB6612_PIN_IN2:
        pin = (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_AIN2;
        break;
    case TB6612_PIN_IN3:
        pin = (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_BIN1;
        break;
    case TB6612_PIN_IN4:
        pin = (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_BIN2;
        break;
    case TB6612_PIN_STBY:
        pin = (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_STBY;
        break;
    default:
        return;
    }
    if (pin < 0) {
        return;
    }
    (void)GpioWritePin(pin, (u32_t)(level ? 1U : 0U));
}

static void motor_pwm_set(uint8_t channel, uint16_t duty_permille)
{
    u32_t duty_max = (1U << (u32_t)MOTOR_PWM_DUTY_RES) - 1U;
    u32_t duty = ((u32_t)duty_permille * duty_max) / 1000U;
    PwmChannel_t ch = (channel == TB6612_CHANNEL_A) ? PWM_CHANNEL_0_E : PWM_CHANNEL_1_E;

    (void)PwmSetDuty(ch, duty);
}

static bool motor_ensure_ready(void)
{
    GpioPinConfig_t g = {0};
    PwmDriverConfig_t pwm = {0};
    PwmChannelConfig_t ch = {0};
    tb6612_config_t cfg = {0};
    const s32_t pins[] = {
        (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_AIN1,
        (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_AIN2,
        (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_BIN1,
        (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_BIN2,
    };
    size_t i;

    if (s_motor_ready) {
        return true;
    }
    if (GpioDriverInit() != TRUE) {
        return false;
    }
    g.mode       = GPIO_MODE_OUTPUT_E;
    g.pullUpEn   = GPIO_PULL_DISABLE_E;
    g.pullDownEn = GPIO_PULL_DISABLE_E;
    g.intrType   = GPIO_INTR_DISABLE_E;
    for (i = 0; i < (sizeof(pins) / sizeof(pins[0])); i++) {
        g.pin = pins[i];
        if (GpioConfigurePin(&g) != TRUE) {
            return false;
        }
        (void)GpioWritePin(pins[i], 0U);
    }
    if ((s32_t)BOARD_DESKTOP_PET_TB6612_PIN_STBY >= 0) {
        g.pin = (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_STBY;
        if (GpioConfigurePin(&g) != TRUE) {
            return false;
        }
        (void)GpioWritePin(g.pin, 1U);
    }

    pwm.speedMode         = PWM_SPEED_MODE_LOW_E;
    pwm.timer             = PWM_TIMER_0_E;
    pwm.dutyResolution    = MOTOR_PWM_DUTY_RES;
    pwm.frequencyHz       = MOTOR_PWM_FREQ_HZ;
    if (PwmDriverInit(&pwm) != TRUE) {
        return false;
    }
    ch.timer  = PWM_TIMER_0_E;
    ch.duty   = 0U;
    ch.hpoint = 0;
    ch.channel = PWM_CHANNEL_0_E;
    ch.gpioPin = (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_PWMA;
    if (PwmConfigureChannel(&ch) != TRUE) {
        return false;
    }
    (void)PwmStart(PWM_CHANNEL_0_E);
    ch.channel = PWM_CHANNEL_1_E;
    ch.gpioPin = (s32_t)BOARD_DESKTOP_PET_TB6612_PIN_PWMB;
    if (PwmConfigureChannel(&ch) != TRUE) {
        return false;
    }
    (void)PwmStart(PWM_CHANNEL_1_E);

    cfg.gpio_set           = motor_gpio_set;
    cfg.pwm_set            = motor_pwm_set;
    cfg.get_tick_ms        = NULL;
    cfg.channel_left       = TB6612_CHANNEL_A;
    cfg.channel_right      = TB6612_CHANNEL_B;
    cfg.left_forward_dir   = TB6612_DIR_CW;
    cfg.right_forward_dir  = TB6612_DIR_CW;
    if (tb6612_register(&s_tb6612, &cfg) != TB6612_OK) {
        return false;
    }
    s_motor_ready = true;
    return true;
}
#endif /* DESKTOP_PET_ENABLE_MOTOR */

static bool selftest_motor(void)
{
#if !DESKTOP_PET_ENABLE_MOTOR
    selftest_log_result("motor", false, "ENABLE_MOTOR=0");
    return false;
#else
    if (!motor_ensure_ready()) {
        selftest_log_result("motor", false, "init");
        return false;
    }

    /* 短时双轮前进，确认两侧都能转。 */
    (void)tb6612_car_forward(&s_tb6612, MOTOR_TEST_SPEED_PERMILLE);
    vTaskDelay(pdMS_TO_TICKS(MOTOR_TEST_MS));
    (void)tb6612_car_stop(&s_tb6612, true);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 左轮前转 + 右轮后转，持续 5s（spin_right）。 */
    (void)tb6612_car_spin_right(&s_tb6612, MOTOR_TEST_SPEED_PERMILLE);
    vTaskDelay(pdMS_TO_TICKS(MOTOR_SPIN_TEST_MS));
    (void)tb6612_car_stop(&s_tb6612, true);

    selftest_log_result("motor", true, "fwd + Lfwd/Rrev 5s");
    return true;
#endif
}

static bool selftest_sd(void)
{
#if !DESKTOP_PET_ENABLE_SDCARD
    selftest_log_result("sd", false, "ENABLE_SDCARD=0");
    return false;
#else
    if (sdcard_get_card() == NULL) {
        if (sdcard_mount(BOARD_SDCARD_MOUNT_POINT) != STATUS_OK) {
            selftest_log_result("sd", false, "mount");
            return false;
        }
    }
    sdcard_mount_smoke_and_benchmark_log();
    selftest_log_result("sd", true, "fat smoke");
    return true;
#endif
}

static bool selftest_audio(void)
{
#if !DESKTOP_PET_ENABLE_AUDIO
    selftest_log_result("audio", false, "ENABLE_AUDIO=0");
    return false;
#else
    GpioPinConfig_t g = {0};

    if (GpioDriverInit() != TRUE) {
        selftest_log_result("audio", false, "gpio");
        return false;
    }
    g.pin        = (s32_t)BOARD_DESKTOP_PET_PA_EN_PIN;
    g.mode       = GPIO_MODE_OUTPUT_E;
    g.pullUpEn   = GPIO_PULL_DISABLE_E;
    g.pullDownEn = GPIO_PULL_DISABLE_E;
    g.intrType   = GPIO_INTR_DISABLE_E;
    if (GpioConfigurePin(&g) != TRUE) {
        selftest_log_result("audio", false, "pa_en cfg");
        return false;
    }
    (void)GpioWritePin(g.pin, (u32_t)BOARD_DESKTOP_PET_PA_EN_ACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(200));
    (void)GpioWritePin(g.pin, 0U);
    selftest_log_result("audio", true, "SPE_EN pulse (codec I2S TBD)");
    return true;
#endif
}

static bool selftest_touch_probe(void)
{
#if !DESKTOP_PET_ENABLE_TOUCH
    selftest_log_result("touch", false, "ENABLE_TOUCH=0");
    return false;
#else
    /*
     * IT7259 @ 0x46：I2cProbe 不依赖注册；I2cWriteRead 必须先 RegisterDevice。
     * Query Buffer(mode=0x80) 在芯片 busy 时会 NACK，手册要求重试。
     */
    I2cDeviceConfig_t cfg = {0};
    uint8_t query_mode = 0x80U;
    uint8_t query = 0U;
    bool probed;
    bool query_ok = false;
    unsigned attempt;
    char detail[72];

    probed = (I2cProbe((s32_t)BOARD_DESKTOP_PET_TOUCH_I2C_PORT,
                       (u16_t)BOARD_DESKTOP_PET_TOUCH_I2C_ADDR) == TRUE);
    if (!probed) {
        selftest_log_result("touch", false, "IT7259 addr=0x46 probe=0 (check I2C/solder)");
        return false;
    }

    cfg.port = (s32_t)BOARD_DESKTOP_PET_TOUCH_I2C_PORT;
    cfg.deviceAddress7bit = (u16_t)BOARD_DESKTOP_PET_TOUCH_I2C_ADDR;
    cfg.clockSpeedHz = 0U;
    cfg.transactionTimeoutMs = 0U;
    (void)I2cRegisterDevice(&cfg);

    for (attempt = 0U; attempt < 10U; attempt++) {
        if (I2cWriteRead((s32_t)BOARD_DESKTOP_PET_TOUCH_I2C_PORT,
                         (u16_t)BOARD_DESKTOP_PET_TOUCH_I2C_ADDR,
                         &query_mode,
                         1U,
                         &query,
                         1U) == TRUE) {
            query_ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    (void)snprintf(detail, sizeof(detail), "IT7259 addr=0x%02X probe=1 query%s=0x%02X try=%u",
                   (unsigned)BOARD_DESKTOP_PET_TOUCH_I2C_ADDR,
                   query_ok ? "" : "(nack)",
                   (unsigned)query,
                   (unsigned)(attempt + 1U));
    /* 地址已 ACK 即认为硬件在位；Query 成功则协议也通。 */
    selftest_log_result("touch", true, detail);
    if (!query_ok) {
        LOG_WARN("selftest touch: Query Buffer still NACK (chip busy?) — not a solder fault");
    }
    return true;
#endif
}

static void selftest_run_all(void)
{
    unsigned pass = 0;
    unsigned total = 0;

    LOG_INFO("selftest: begin (lcd=%d imu=%d motor=%d sd=%d audio=%d touch=%d)",
             DESKTOP_PET_ENABLE_LCD,
             DESKTOP_PET_ENABLE_IMU,
             DESKTOP_PET_ENABLE_MOTOR,
             DESKTOP_PET_ENABLE_SDCARD,
             DESKTOP_PET_ENABLE_AUDIO,
             DESKTOP_PET_ENABLE_TOUCH);

    total++;
    if (selftest_i2c()) {
        pass++;
    }
    total++;
    if (selftest_bat()) {
        pass++;
    }
    total++;
    if (selftest_lcd()) {
        pass++;
    }
    total++;
    if (selftest_imu()) {
        pass++;
    }
    total++;
    if (selftest_led()) {
        pass++;
    }
    total++;
    if (selftest_motor()) {
        pass++;
    }
    total++;
    if (selftest_sd()) {
        pass++;
    }
    total++;
    if (selftest_audio()) {
        pass++;
    }
    total++;
    if (selftest_touch_probe()) {
        pass++;
    }

    LOG_INFO("selftest: done pass=%u/%u", pass, total);
}

static void cmd_ptest(int argc, const char *argv[])
{
    const char *sub = (argc >= 2) ? argv[1] : "all";

    if (strcmp(sub, "all") == 0) {
        selftest_run_all();
        cmd_reply_ok("ptest", "all");
        return;
    }
    if (strcmp(sub, "i2c") == 0) {
        cmd_reply_ok("ptest", selftest_i2c() ? "ok" : "ng");
        return;
    }
    if (strcmp(sub, "bat") == 0) {
        cmd_reply_ok("ptest", selftest_bat() ? "ok" : "ng");
        return;
    }
    if (strcmp(sub, "lcd") == 0) {
        cmd_reply_ok("ptest", selftest_lcd() ? "ok" : "ng");
        return;
    }
    if (strcmp(sub, "imu") == 0) {
        cmd_reply_ok("ptest", selftest_imu() ? "ok" : "ng");
        return;
    }
    if (strcmp(sub, "led") == 0) {
        cmd_reply_ok("ptest", selftest_led() ? "ok" : "ng");
        return;
    }
    if (strcmp(sub, "motor") == 0) {
        cmd_reply_ok("ptest", selftest_motor() ? "ok" : "ng");
        return;
    }
    if (strcmp(sub, "sd") == 0) {
        cmd_reply_ok("ptest", selftest_sd() ? "ok" : "ng");
        return;
    }
    if (strcmp(sub, "audio") == 0) {
        cmd_reply_ok("ptest", selftest_audio() ? "ok" : "ng");
        return;
    }
    if (strcmp(sub, "touch") == 0) {
        cmd_reply_ok("ptest", selftest_touch_probe() ? "ok" : "ng");
        return;
    }
    cmd_reply_ng();
}

static void selftest_register_cmds(void)
{
    (void)cmd_register("ptest", cmd_ptest, "pet selftest [all|lcd|bat|imu|led|motor|sd|i2c|audio|touch]");
}

#if DESKTOP_PET_ENABLE_SELFTEST && DESKTOP_PET_SELFTEST_BOOT_RUN
static void selftest_boot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    selftest_run_all();
    vTaskDelete(NULL);
}
#endif

status_t desktop_pet_selftest_start(void)
{
#if !DESKTOP_PET_ENABLE_SELFTEST
    LOG_INFO("selftest disabled (DESKTOP_PET_ENABLE_SELFTEST=0)");
    return STATUS_OK;
#else
    cmd_set_project_register_fn(selftest_register_cmds);
    if (cmd_usb_line_service_start() != STATUS_OK) {
        LOG_WARN("cmd_usb_line_service_start failed");
    } else {
        LOG_INFO("USB cmd ready: type 'help' / 'ptest'");
    }

#if DESKTOP_PET_SELFTEST_BOOT_RUN
    if (xTaskCreate(selftest_boot_task, "pet_test", SELFTEST_TASK_STACK_WORDS, NULL, SELFTEST_TASK_PRIORITY,
                    NULL) != pdPASS) {
        LOG_WARN("selftest boot task create failed");
        return STATUS_FAIL;
    }
#endif
    return STATUS_OK;
#endif
}
