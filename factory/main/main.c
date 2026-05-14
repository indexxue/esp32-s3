/**
 * @file main.c
 * @brief 厂测固件入口。量产业务（LCD/SD/IMU 等）在 `project/main`；本目录仅厂测与共用 BSP/common。
 */

#include "factory.h"

#include "log.h"

void app_main(void)
{
    status_t err = factory_entry();

    if (err != STATUS_OK) {
        LOG_ERROR("factory startup failed: %s", status_to_str(err));
    }
}
