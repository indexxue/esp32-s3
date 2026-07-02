/**
 * @file voice_hub_storage.h
 * @brief 1-bit SDMMC 存储骨架（M4）。
 */

#pragma once

#include "type.h"

#include <stdint.h>

status_t voice_hub_storage_init(void);
bool_t voice_hub_storage_is_mounted(void);
/** FAT 短文件写读校验；成功后再打 DMA 吞吐日志。 */
status_t voice_hub_storage_run_rw_test(void);
/** 在 Wi-Fi 启动前调用 init/rw_test 后，供 LCD 显示的状态文案。 */
const char *voice_hub_storage_boot_status_line(void);
status_t voice_hub_storage_write_jpeg_snapshot(const uint8_t *data, uint32_t len);
