#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpio.h"

#define DEMO_TOGGLE_GPIO (2)
#define DEMO_TOGGLE_PERIOD_MS 500U
#define DEMO_LOG_PERIOD_MS 1000U

static void LedBlinkTask(void *args)
{
    (void)args;

    while (1) {
        if (GpioTogglePin(DEMO_TOGGLE_GPIO) == FALSE) {
            printf("GpioTogglePin failed, err=%ld\n", (long)GpioGetLastError());
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(DEMO_TOGGLE_PERIOD_MS));
    }
}

static void AliveLogTask(void *args)
{
    (void)args;

    u32_t counter = 0U;
    while (1) {
        counter++;
        printf("[FreeRTOS] alive counter=%lu\n", (unsigned long)counter);
        vTaskDelay(pdMS_TO_TICKS(DEMO_LOG_PERIOD_MS));
    }
}

void app_main(void)
{
    GpioPinConfig_t ledPinCfg = {
        .pin = DEMO_TOGGLE_GPIO,
        .mode = GPIO_MODE_OUTPUT_E,
        .pullUpEn = GPIO_PULL_DISABLE_E,
        .pullDownEn = GPIO_PULL_DISABLE_E,
        .intrType = GPIO_INTR_DISABLE_E,
    };

    if (GpioDriverInit() == FALSE) {
        printf("GpioDriverInit failed, err=%ld\n", (long)GpioGetLastError());
        return;
    }

    if (GpioConfigurePin(&ledPinCfg) == FALSE) {
        printf("GpioConfigurePin failed, err=%ld\n", (long)GpioGetLastError());
        return;
    }

    BaseType_t ret1 = xTaskCreate(LedBlinkTask, "led_blink_task", 2048, NULL, 5, NULL);
    BaseType_t ret2 = xTaskCreate(AliveLogTask, "alive_log_task", 2048, NULL, 4, NULL);

    if ((ret1 != pdPASS) || (ret2 != pdPASS)) {
        printf("xTaskCreate failed: led=%ld log=%ld\n", (long)ret1, (long)ret2);
    }
}
