#ifndef DRIVER_SPI_H
#define DRIVER_SPI_H

#include "type.h"

typedef enum {
    SPI_HOST_2_E = 2,
    SPI_HOST_3_E = 3
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
    s32_t dmaChannel;
    s32_t intrFlags;
    u8_t maxDeviceCount;
} SpiDriverConfig_t;

typedef struct {
    s32_t chipSelectPin;
    u32_t clockSpeedHz;
    SpiClockMode_t mode;
    u32_t flags;
    u8_t queueSize;
} SpiDeviceConfig_t;

bool_t SpiDriverInit(const SpiDriverConfig_t *config);
bool_t SpiDriverDeinit(void);

bool_t SpiRegisterDevice(const SpiDeviceConfig_t *config);
bool_t SpiUnregisterDevice(s32_t chipSelectPin);

bool_t SpiTransmit(s32_t chipSelectPin, const u8_t *txBuffer, usize_t txLength);
bool_t SpiReceive(s32_t chipSelectPin, u8_t *rxBuffer, usize_t rxLength);
bool_t SpiTransmitReceive(s32_t chipSelectPin, const u8_t *txBuffer, u8_t *rxBuffer, usize_t length);

s32_t SpiGetLastError(void);

#endif
