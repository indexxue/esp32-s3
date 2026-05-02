#include "board.h"

#include "gpio.h"
#include "i2c.h"
#include "pwm.h"

static bool_t s_gpioReady = FALSE;
static bool_t s_pwmReady = FALSE;
static bool_t s_i2cReady = FALSE;

bool_t BoardInit(void)
{
    GpioPinConfig_t const io5Cfg = {
        .pin = BOARD_GPIO_IO5,
        .mode = GPIO_MODE_OUTPUT_E,
        .pullUpEn = GPIO_PULL_DISABLE_E,
        .pullDownEn = GPIO_PULL_DISABLE_E,
        .intrType = GPIO_INTR_DISABLE_E,
    };

    if (GpioDriverInit() == FALSE) {
        return FALSE;
    }
    s_gpioReady = TRUE;

    if (GpioConfigurePin(&io5Cfg) == FALSE) {
        BoardDeinit();
        return FALSE;
    }

    {
        PwmDriverConfig_t const pwmDrv = {
            .speedMode = PWM_SPEED_MODE_LOW_E,
            .timer = PWM_TIMER_0_E,
            .dutyResolution = PWM_DUTY_RES_10BIT_E,
            .frequencyHz = BOARD_IO4_PWM_FREQ_HZ,
        };
        if (PwmDriverInit(&pwmDrv) == FALSE) {
            BoardDeinit();
            return FALSE;
        }
    }
    s_pwmReady = TRUE;

    {
        PwmChannelConfig_t const pwmCh = {
            .channel = PWM_CHANNEL_0_E,
            .timer = PWM_TIMER_0_E,
            .gpioPin = BOARD_GPIO_IO4_PWM,
            .duty = 0U,
            .hpoint = 0,
        };
        if (PwmConfigureChannel(&pwmCh) == FALSE) {
            BoardDeinit();
            return FALSE;
        }
    }
    if (PwmStart(PWM_CHANNEL_0_E) == FALSE) {
        BoardDeinit();
        return FALSE;
    }

    {
        I2cDriverConfig_t const i2cCfg = {
            .port = BOARD_I2C_PORT,
            .sdaPin = BOARD_I2C_SDA_GPIO,
            .sclPin = BOARD_I2C_SCL_GPIO,
            .defaultClockSpeedHz = 100000U,
            .defaultTransactionTimeoutMs = 50U,
            .maxDeviceCount = BOARD_I2C_MAX_DEVICE_SLOTS,
            .glitchIgnoreCount = 7U,
            .enableSdaPullup = TRUE,
            .enableSclPullup = TRUE,
        };
        if (I2cDriverInit(&i2cCfg) == FALSE) {
            BoardDeinit();
            return FALSE;
        }
    }
    s_i2cReady = TRUE;

    return TRUE;
}

void BoardDeinit(void)
{
    if (s_i2cReady != FALSE) {
        (void)I2cDriverDeinit();
        s_i2cReady = FALSE;
    }
    if (s_pwmReady != FALSE) {
        (void)PwmDriverDeinit();
        s_pwmReady = FALSE;
    }
    if (s_gpioReady != FALSE) {
        (void)GpioDriverDeinit();
        s_gpioReady = FALSE;
    }
}

void BoardDeinitI2cBus(void)
{
    if (s_i2cReady != FALSE) {
        (void)I2cDriverDeinit();
        s_i2cReady = FALSE;
    }
}
