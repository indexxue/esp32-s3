#ifndef DRIVER_SDIO_H
#define DRIVER_SDIO_H

#include "dma.h"
#include "driver/sdmmc_host.h"
#include "type.h"

/**
 * SDMMC 接线（ESP32-S3 可通过 GPIO 矩阵映射到任意空闲 GPIO；1 线模式仅使用 CLK/CMD/D0）。
 */
typedef struct {
    s32_t pin_clk;
    s32_t pin_cmd;
    s32_t pin_d0;
    /** 4 线模式使用；1 线时可填 -1（GPIO_NUM_NC）。 */
    s32_t pin_d1;
    s32_t pin_d2;
    s32_t pin_d3;
    /** 1 或 4。 */
    u8_t bus_width;
    /** 0 表示使用 `SDMMC_HOST_DEFAULT()` 中的默认频率。 */
    u32_t max_freq_khz;
    /** SDMMC 使用主机内置 DMA 时的路径枚举（当前仅 IDMAC）。 */
    DmaSdmmcPath_t sdmmc_dma_path;
    /** 与 `SDMMC_HOST_DEFAULT()` 的 `flags` 按位或，例如 `SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF`（SDIO 字节模式缓存后缓冲区）。 */
    u32_t host_flags_or;
} SdioBoardConfig_t;

/**
 * 按板级接线填充 `sdmmc_host_t` 与 `sdmmc_slot_config_t`，供 `esp_vfs_fat_sdmmc_mount` 等上层使用。
 * 不会执行 `host->init()` / `sdmmc_host_init_slot`（由 FAT 挂载或自行调用完成）。
 */
bool_t SdioFillSdmmcForBoard(sdmmc_host_t *host, sdmmc_slot_config_t *slot, const SdioBoardConfig_t *cfg);

s32_t SdioGetLastError(void);

#endif
