/**
 * @file start.c
 * @brief camera 生命周期编排：init → run。
 */

#include "start.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_app_boot.h"
#include "camera_app_config.h"
#include "camera_app_input.h"
#include "device_profile.h"
#include "log.h"

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
    return STATUS_OK;
}

static status_t app_init(void)
{
    status_t err;
    const device_product_profile_t *product = device_profile_product();

    err = camera_app_boot_init();
    if (err != STATUS_OK) {
        return err;
    }

    err = camera_app_pwr_relay_init();
    if (err != STATUS_OK) {
        return err;
    }

    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_BUTTON)) {
        err = camera_app_button_init();
        if (err != STATUS_OK) {
            return err;
        }
    }

    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED)) {
        err = camera_app_led_init();
        if (err != STATUS_OK) {
            return err;
        }
    }

    err = camera_app_calib_cmd_init();
    if (err != STATUS_OK) {
        return err;
    }

    LOG_INFO("%s ready detect=%d collect=%d calib=%d (platform_mask=0x%02lX)",
             product->name,
             CAMERA_APP_DETECT_MODE,
             CAMERA_APP_COLLECT_MODE,
             CAMERA_APP_CALIB_MODE,
             (unsigned long)device_profile_platform_mask());
    return STATUS_OK;
}

static void app_run(void)
{
    app_idle_default();
}

static const app_lifecycle_t s_default_lifecycle = {
    .init = app_init,
    .run  = app_run,
};

status_t app_entry(void)
{
    return app_start(&s_default_lifecycle);
}
