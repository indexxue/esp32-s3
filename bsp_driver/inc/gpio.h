#ifndef DRIVER_GPIO_H
#define DRIVER_GPIO_H

#include "type.h"

typedef struct {
    s32_t pin;
    s32_t mode;
    s32_t pullUpEn;
    s32_t pullDownEn;
    s32_t intrType;
} GpioPinConfig_t;

typedef void (*GpioIsrHandler_t)(void_t *args);

typedef enum {
    GPIO_MODE_DISABLE_E = 0,
    GPIO_MODE_INPUT_E = 1,
    GPIO_MODE_OUTPUT_E = 2,
    GPIO_MODE_INPUT_OUTPUT_E = 3,
    GPIO_MODE_OUTPUT_OD_E = 4,
    GPIO_MODE_INPUT_OUTPUT_OD_E = 5
} GpioMode_t;

typedef enum {
    GPIO_PULL_DISABLE_E = 0,
    GPIO_PULL_ENABLE_E = 1
} GpioPull_t;

typedef enum {
    GPIO_INTR_DISABLE_E = 0,
    GPIO_INTR_POSEDGE_E = 1,
    GPIO_INTR_NEGEDGE_E = 2,
    GPIO_INTR_ANYEDGE_E = 3,
    GPIO_INTR_LOW_LEVEL_E = 4,
    GPIO_INTR_HIGH_LEVEL_E = 5
} GpioIntrType_t;

bool_t GpioDriverInit(void);
bool_t GpioDriverDeinit(void);

bool_t GpioConfigurePin(const GpioPinConfig_t *config);
bool_t GpioWritePin(s32_t pin, u32_t level);
bool_t GpioReadPin(s32_t pin, u32_t *level);
bool_t GpioTogglePin(s32_t pin);

bool_t GpioRegisterIsr(s32_t pin, GpioIsrHandler_t isrHandler, void_t *args);
bool_t GpioUnregisterIsr(s32_t pin);
s32_t GpioGetLastError(void);

#endif
