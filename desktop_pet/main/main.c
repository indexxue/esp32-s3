/**
 * @file main.c
 * @brief desktop_pet 应用入口；业务见 start.c，外设引脚见 board.h BOARD_PROFILE_DESKTOP_PET。
 */

#include "start.h"

#include "log.h"

void app_main(void)
{
    status_t err = app_entry();

    if (err != STATUS_OK) {
        LOG_ERROR("desktop_pet startup failed: %s", status_to_str(err));
    }
}
