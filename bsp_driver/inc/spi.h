#ifndef DRIVER_SPI_H
#define DRIVER_SPI_H

#include "dma.h"
#include "type.h"

/**
 * 与 ESP-IDF `spi_host_device_t` 数值一致（ESP32-S3：SPI2=1，SPI3=2）。
 * 勿写成外设序号 2/3，否则 host_id=3 会 spi_bus_initialize 失败。
 */
typedef enum {
    SPI_HOST_2_E = 1,
    SPI_HOST_3_E = 2
} SpiHost_t;

typedef enum {
    SPI_CLOCK_MODE_0_E = 0,
    SPI_CLOCK_MODE_1_E = 1,
    SPI_CLOCK_MODE_2_E = 2,
    SPI_CLOCK_MODE_3_E = 3
} SpiClockMode_t;

typedef struct {
    SpiHost_t host;
    s32_t sclkPin;
    s32_t mosiPin;
    s32_t misoPin;
    s32_t quadWpPin;
    s32_t quadHdPin;
    s32_t maxTransferSize;
    DmaSpiBusChannel_t dmaChannel;
    s32_t intrFlags;
    u8_t maxDeviceCount;
} SpiDriverConfig_t;

/**
 * 设备片选脚号；与 `SpiTransmit` / `SpiUnregisterDevice` 的 `chipSelectPin` 一致。
 * 传 `-1`（`GPIO_NUM_NC`）表示不由硬件自动控制 CS，由上层在两次 `SpiTransmit` 之间用 GPIO 保持/释放片选
 * （例如 ST7789/ILI9341 在 `RAMWR` 后连续写像素）。
 * `host` 必须与已 `SpiDriverInit` 的总线一致（支持 SPI2+SPI3 并存）。
 */
typedef struct {
    SpiHost_t host;
    s32_t chipSelectPin;
    u32_t clockSpeedHz;
    SpiClockMode_t mode;
    u32_t flags;
    u8_t queueSize;
    /** CS 拉低后、首时钟前的 SPI bit 周期数（1 MHz 时约等于 µs）。 */
    u8_t csEnaPretrans;
    /** 末位后 CS 保持低的 SPI bit 周期数。 */
    u8_t csEnaPosttrans;
} SpiDeviceConfig_t;

/** 初始化指定 host 总线；可对 SPI2 / SPI3 各调用一次。同 host 重复 init 视为成功。 */
bool_t SpiDriverInit(const SpiDriverConfig_t *config);
/** 释放所有已初始化的 SPI host。 */
bool_t SpiDriverDeinit(void);
bool_t SpiDriverDeinitHost(SpiHost_t host);

bool_t SpiRegisterDevice(const SpiDeviceConfig_t *config);
bool_t SpiUnregisterDevice(s32_t chipSelectPin);

bool_t SpiTransmit(s32_t chipSelectPin, const u8_t *txBuffer, usize_t txLength);
bool_t SpiReceive(s32_t chipSelectPin, u8_t *rxBuffer, usize_t rxLength);
bool_t SpiTransmitReceive(s32_t chipSelectPin, const u8_t *txBuffer, u8_t *rxBuffer, usize_t length);

/** 要求 TX 缓冲区落在 DMA 可达内存上；总线已启用 DMA 时用于大块传输。 */
bool_t SpiTransmitDma(s32_t chipSelectPin, const u8_t *txBuffer, usize_t txLength);
bool_t SpiReceiveDma(s32_t chipSelectPin, u8_t *rxBuffer, usize_t rxLength);
bool_t SpiTransmitReceiveDma(s32_t chipSelectPin, const u8_t *txBuffer, u8_t *rxBuffer, usize_t length);

s32_t SpiGetLastError(void);

#endif
