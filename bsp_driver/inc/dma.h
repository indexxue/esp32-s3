#ifndef DRIVER_DMA_H
#define DRIVER_DMA_H

#include "type.h"

/**
 * @file dma.h
 * @brief 片上外设 DMA 相关 BSP：SPI 总线 DMA 通道枚举与 SDMMC 路径说明、DMA 缓冲区能力检查与分配。
 */

/** SPI 主机总线初始化时使用的 DMA 通道策略（见 `spi_bus_initialize` 第三参）。 */
typedef enum {
    /** 不使用 DMA（与 `SPI_DMA_DISABLED` 一致）。 */
    DMA_SPI_BUS_DISABLED_E = 0,
    /**
     * 由驱动自动选择 DMA 通道（与 `SPI_DMA_CH_AUTO` 一致，ESP32-S3 上为 3）。
     * 具体枚举值以 `driver/spi_master.h` / SoC 头为准；此处仅保证与当前 IDF 常用配置一致。
     */
    DMA_SPI_BUS_AUTO_E = 3
} DmaSpiBusChannel_t;

/**
 * SDMMC/SDIO 数据传输路径（与 SPI 的“GDMA 通道号”不同）。
 * ESP32-S3 上 SDMMC 主机使用控制器内置 IDMAC（环形描述符，数量由 ESP-IDF 驱动固定），应用层不能指定独立 GDMA 通道索引。
 */
typedef enum {
    /** 使用主机内置 IDMAC（与 `esp_driver_sdmmc` 默认行为一致）。 */
    DMA_SDMMC_PATH_CONTROLLER_IDMAC_E = 0
} DmaSdmmcPath_t;

bool_t DmaSpiBusChannelIsValid(DmaSpiBusChannel_t channel);

bool_t DmaSdmmcPathIsValid(DmaSdmmcPath_t path);

/**
 * 转换为 `spi_bus_initialize(..., dma_chan)` 可用的整型值（与 `spi_dma_chan_t` 二进制兼容）。
 * 非法 `channel` 时返回 `-1`。
 */
s32_t DmaSpiBusChannelToSpiBusInitValue(DmaSpiBusChannel_t channel);

/**
 * 判断连续缓冲区是否落在可参与 SPI 等总线 DMA 的内存上（起止地址均检查）。
 * @param buffer 缓冲区首地址
 * @param length 字节长度
 */
bool_t DmaBufferIsBusCapable(const void *buffer, usize_t length);

/**
 * 分配 DMA 可访问的内存（`MALLOC_CAP_DMA`），用于大块 SPI 发送等场景。
 * @return 成功返回指针，失败返回 NULL_PTR
 */
void *DmaMalloc(usize_t size);

void DmaFree(void *ptr);

#endif
