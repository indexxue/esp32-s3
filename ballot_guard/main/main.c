/**
 * @file main.c
 * @brief 选票识别与全流程监管系统 — 应用入口。
 *        工程名 ballot_guard；烧录默认写入 app_a（ota_0），NVS 与 project/ 共用分区表。
 */

#include "start.h"

#include "log.h"

void app_main(void)
{
    status_t err = app_entry();

    if (err != STATUS_OK) {
        LOG_ERROR("ballot_guard startup failed: %s", status_to_str(err));
    }
}
