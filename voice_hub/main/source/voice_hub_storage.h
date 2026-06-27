/**
 * @file voice_hub_storage.h
 * @brief 1-bit SDMMC 存储骨架（M4）。
 */

#pragma once

#include "type.h"

#include <stdint.h>

status_t voice_hub_storage_init(void);
bool_t voice_hub_storage_is_mounted(void);
status_t voice_hub_storage_write_jpeg_snapshot(const uint8_t *data, uint32_t len);
