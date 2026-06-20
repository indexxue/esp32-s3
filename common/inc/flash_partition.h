/**
 * @file    flash_partition.h
 * @brief   ESP32-S3 N16R8 片内 Flash 分区常量（与 flash_partition/partitions_16m_n16r8.csv 一致）
 *
 * 日常读写请用 esp_partition_* / esp_ota_*；本头文件供脚本、评审及与 STM32 工程 flash_partition 习惯对齐。
 */

#ifndef COMMON_FLASH_PARTITION_H
#define COMMON_FLASH_PARTITION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* flash_partition/partitions_16m_n16r8 — 与 partitions_16m_n16r8.csv 一致 */
#define FLASH_CHIP_SIZE (0x01000000u) /* 16 MiB */

#define FLASH_BOOTLOADER_START (0x00000000u)
#define FLASH_BOOTLOADER_END (0x00008000u) /* 不含；与 IDF 默认 32KiB 窗口对齐 */

#define FLASH_PART_TABLE_START (0x00008000u)
#define FLASH_PART_TABLE_END (0x00009000u) /* 不含 */

#define PART_NVS_START (0x00009000u)
#define PART_NVS_SIZE (0x00020000u)
#define PART_NVS_END (PART_NVS_START + PART_NVS_SIZE)

#define PART_OTADATA_START (0x00029000u)
#define PART_OTADATA_SIZE (0x00002000u)
#define PART_OTADATA_END (PART_OTADATA_START + PART_OTADATA_SIZE)

#define PART_PHY_INIT_START (0x0002B000u)
#define PART_PHY_INIT_SIZE (0x00001000u)
#define PART_PHY_INIT_END (PART_PHY_INIT_START + PART_PHY_INIT_SIZE)

#define PART_APP_ALIGN_GAP_START (0x0002C000u) /* phy 后至首 app 前的 64KiB 对齐留白 */
#define PART_APP_ALIGN_GAP_SIZE (0x00004000u)

#define PART_APP_A_START (0x00030000u)
#define PART_APP_A_SIZE (0x007E0000u)
#define PART_APP_A_END (PART_APP_A_START + PART_APP_A_SIZE)

#define PART_APP_B_START (0x00810000u)
#define PART_APP_B_SIZE (0x007E0000u)
#define PART_APP_B_END (PART_APP_B_START + PART_APP_B_SIZE)

/* 未分区留白：PART_APP_B_END .. FLASH_CHIP_SIZE（当前为 0x10000 字节） */

/* 与 STM32 工程 tmp/nvs.h 命名对齐的别名 */
#define FLASH_PART_NVS_START PART_NVS_START
#define FLASH_PART_NVS_SIZE PART_NVS_SIZE
#define FLASH_PART_NVS_END PART_NVS_END

#define FLASH_PART_APP_A_START PART_APP_A_START
#define FLASH_PART_APP_A_SIZE PART_APP_A_SIZE
#define FLASH_PART_APP_B_START PART_APP_B_START
#define FLASH_PART_APP_B_SIZE PART_APP_B_SIZE

_Static_assert(PART_APP_B_END <= FLASH_CHIP_SIZE, "partition map must not exceed flash top");

#ifdef __cplusplus
}
#endif

#endif /* COMMON_FLASH_PARTITION_H */
