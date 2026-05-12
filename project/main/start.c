/**
 * @file start.c
 * @brief 生命周期调度：与具体业务解耦，仅负责顺序与默认 idle。
 */

#include "start.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void app_idle_default(void)
{
    const TickType_t period = pdMS_TO_TICKS(APP_LIFECYCLE_IDLE_DELAY_MS);

    for (;;) {
        vTaskDelay(period);
    }
}

status_t app_start(const app_lifecycle_t *lifecycle)
{
    status_t err;

    if (lifecycle == NULL) {
        return STATUS_INVALID_ARG;
    }

    if (lifecycle->init != NULL) {
        err = lifecycle->init();
        if (err != STATUS_OK) {
            return err;
        }
    }

    if (lifecycle->run != NULL) {
        lifecycle->run();
        return STATUS_OK;
    }

    app_idle_default();
    /* 不可达，满足部分静态分析工具 */
    return STATUS_OK;
}
