/**
 * @file boot_slot.h
 * @brief 下次启动槽选择（ota_0 / ota_1），语义对齐 `doc/partition_switch_development_plan.md` 与 STM32 `boot_slot_*`。
 *
 * 通过 `esp_ota_set_boot_partition()` 写 `otadata`，勿在应用层直接擦写 `otadata` 扇区。
 */

#ifndef COMMON_BOOT_SLOT_H
#define COMMON_BOOT_SLOT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "type.h"

#include <stddef.h>

/** 将下次启动设为 App_A（分区子类型 `ota_0`，CSV 名 `app_a`）。 */
status_t boot_slot_request_app_a(void);

/** 将下次启动设为厂测 / App_B（`ota_1` / `app_b`）。 */
status_t boot_slot_request_factory(void);

/** 软件复位（切换后调用以使 Bootloader 重新选槽）。 */
void boot_slot_system_reset(void);

/**
 * @brief 将当前运行槽与已配置的下次启动槽写入可读串（如 `run=ota_0 next=ota_1`）。
 * @return `STATUS_OK` 或 `STATUS_INVALID_ARG`。
 */
status_t boot_slot_format_status(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_BOOT_SLOT_H */
