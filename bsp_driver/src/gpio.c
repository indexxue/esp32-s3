#include "gpio.h"

#include <stddef.h>

#include "driver/gpio.h"

static bool_t s_gpioDriverInited = FALSE;
static bool_t s_isrServiceInstalled = FALSE;
static esp_err_t s_gpioLastErr = ESP_OK;

static bool_t gpioSetLastErr(esp_err_t err)
{
    s_gpioLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

static bool_t gpioCheckPinValid(s32_t pin)
{
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)pin)) {
        return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return gpioSetLastErr(ESP_OK);
}

static bool_t gpioConvertMode(s32_t mode, gpio_mode_t *outMode)
{
    if (outMode == NULL) {
        return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (mode) {
        case GPIO_MODE_DISABLE_E: *outMode = GPIO_MODE_DISABLE; break;
        case GPIO_MODE_INPUT_E: *outMode = GPIO_MODE_INPUT; break;
        case GPIO_MODE_OUTPUT_E: *outMode = GPIO_MODE_OUTPUT; break;
        case GPIO_MODE_INPUT_OUTPUT_E: *outMode = GPIO_MODE_INPUT_OUTPUT; break;
        case GPIO_MODE_OUTPUT_OD_E: *outMode = GPIO_MODE_OUTPUT_OD; break;
        case GPIO_MODE_INPUT_OUTPUT_OD_E: *outMode = GPIO_MODE_INPUT_OUTPUT_OD; break;
        default: return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return gpioSetLastErr(ESP_OK);
}

static bool_t gpioConvertPullup(s32_t pull, gpio_pullup_t *outPullup)
{
    if (outPullup == NULL) {
        return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (pull) {
        case GPIO_PULL_DISABLE_E: *outPullup = GPIO_PULLUP_DISABLE; break;
        case GPIO_PULL_ENABLE_E: *outPullup = GPIO_PULLUP_ENABLE; break;
        default: return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return gpioSetLastErr(ESP_OK);
}

static bool_t gpioConvertPulldown(s32_t pull, gpio_pulldown_t *outPulldown)
{
    if (outPulldown == NULL) {
        return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (pull) {
        case GPIO_PULL_DISABLE_E: *outPulldown = GPIO_PULLDOWN_DISABLE; break;
        case GPIO_PULL_ENABLE_E: *outPulldown = GPIO_PULLDOWN_ENABLE; break;
        default: return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return gpioSetLastErr(ESP_OK);
}

static bool_t gpioConvertIntrType(s32_t intrType, gpio_int_type_t *outIntrType)
{
    if (outIntrType == NULL) {
        return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (intrType) {
        case GPIO_INTR_DISABLE_E: *outIntrType = GPIO_INTR_DISABLE; break;
        case GPIO_INTR_POSEDGE_E: *outIntrType = GPIO_INTR_POSEDGE; break;
        case GPIO_INTR_NEGEDGE_E: *outIntrType = GPIO_INTR_NEGEDGE; break;
        case GPIO_INTR_ANYEDGE_E: *outIntrType = GPIO_INTR_ANYEDGE; break;
        case GPIO_INTR_LOW_LEVEL_E: *outIntrType = GPIO_INTR_LOW_LEVEL; break;
        case GPIO_INTR_HIGH_LEVEL_E: *outIntrType = GPIO_INTR_HIGH_LEVEL; break;
        default: return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return gpioSetLastErr(ESP_OK);
}

bool_t GpioDriverInit(void)
{
    if (s_gpioDriverInited) {
        return gpioSetLastErr(ESP_OK);
    }

    s_gpioDriverInited = TRUE;
    return gpioSetLastErr(ESP_OK);
}

bool_t GpioDriverDeinit(void)
{
    if (!s_gpioDriverInited) {
        return gpioSetLastErr(ESP_OK);
    }

    if (s_isrServiceInstalled) {
        gpio_uninstall_isr_service();
        s_isrServiceInstalled = FALSE;
    }

    s_gpioDriverInited = FALSE;
    return gpioSetLastErr(ESP_OK);
}

bool_t GpioConfigurePin(const GpioPinConfig_t *config)
{
    gpio_config_t gpioConfig = {0};
    esp_err_t ret = ESP_OK;

    if (!s_gpioDriverInited) {
        return gpioSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (config == NULL) {
        return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }

    if (gpioCheckPinValid(config->pin) == FALSE) {
        return FALSE;
    }

    if (gpioConvertMode(config->mode, &gpioConfig.mode) == FALSE) {
        return FALSE;
    }
    if (gpioConvertPullup(config->pullUpEn, &gpioConfig.pull_up_en) == FALSE) {
        return FALSE;
    }
    if (gpioConvertPulldown(config->pullDownEn, &gpioConfig.pull_down_en) == FALSE) {
        return FALSE;
    }
    if (gpioConvertIntrType(config->intrType, &gpioConfig.intr_type) == FALSE) {
        return FALSE;
    }
    gpioConfig.pin_bit_mask = (1ULL << (u32_t)config->pin);

    ret = gpio_config(&gpioConfig);
    return gpioSetLastErr(ret);
}

bool_t GpioWritePin(s32_t pin, u32_t level)
{
    esp_err_t ret = ESP_OK;

    if (!s_gpioDriverInited) {
        return gpioSetLastErr(ESP_ERR_INVALID_STATE);
    }

    if (gpioCheckPinValid(pin) == FALSE) {
        return FALSE;
    }

    ret = gpio_set_level((gpio_num_t)pin, (level != 0U) ? 1 : 0);
    return gpioSetLastErr(ret);
}

bool_t GpioReadPin(s32_t pin, u32_t *level)
{
    if (!s_gpioDriverInited) {
        return gpioSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (level == NULL) {
        return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }

    if (gpioCheckPinValid(pin) == FALSE) {
        return FALSE;
    }

    *level = (u32_t)gpio_get_level((gpio_num_t)pin);
    return gpioSetLastErr(ESP_OK);
}

bool_t GpioTogglePin(s32_t pin)
{
    u32_t level = 0U;
    if (GpioReadPin(pin, &level) == FALSE) {
        return FALSE;
    }

    return GpioWritePin(pin, (level == 0U) ? 1U : 0U);
}

bool_t GpioRegisterIsr(s32_t pin, GpioIsrHandler_t isrHandler, void_t *args)
{
    esp_err_t ret = ESP_OK;

    if (!s_gpioDriverInited) {
        return gpioSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (isrHandler == NULL) {
        return gpioSetLastErr(ESP_ERR_INVALID_ARG);
    }

    if (gpioCheckPinValid(pin) == FALSE) {
        return FALSE;
    }

    if (!s_isrServiceInstalled) {
        ret = gpio_install_isr_service(0);
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
            return gpioSetLastErr(ret);
        }
        s_isrServiceInstalled = TRUE;
    }

    ret = gpio_isr_handler_add((gpio_num_t)pin, (gpio_isr_t)isrHandler, args);
    return gpioSetLastErr(ret);
}

bool_t GpioUnregisterIsr(s32_t pin)
{
    esp_err_t ret = ESP_OK;

    if (!s_gpioDriverInited) {
        return gpioSetLastErr(ESP_ERR_INVALID_STATE);
    }

    if (gpioCheckPinValid(pin) == FALSE) {
        return FALSE;
    }

    ret = gpio_isr_handler_remove((gpio_num_t)pin);
    return gpioSetLastErr(ret);
}

s32_t GpioGetLastError(void)
{
    return (s32_t)s_gpioLastErr;
}
