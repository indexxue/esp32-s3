/**
 * @file main.c
 * @brief voice_hub 应用入口；业务模块见 voice_hub_* 与 start.c。
 */

#include "start.h"

#include "log.h"

void app_main(void)
{
    status_t err = app_entry();

    if (err != STATUS_OK) {
        LOG_ERROR("voice_hub startup failed: %s", status_to_str(err));
    }
}
