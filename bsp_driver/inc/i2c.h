#ifndef DRIVER_I2C_H
#define DRIVER_I2C_H

#include "type.h"

/** ESP32-S3 上硬件 I2C 控制器数量为 2（I2C_NUM_0 / I2C_NUM_1）。 */
#define BSP_I2C_HW_PORT_COUNT (2)

typedef struct {
    s32_t port;
    s32_t sdaPin;
    s32_t sclPin;
    u32_t defaultClockSpeedHz;
    u32_t defaultTransactionTimeoutMs;
    u8_t maxDeviceCount;
    u8_t glitchIgnoreCount;
    bool_t enableSdaPullup;
    bool_t enableSclPullup;
} I2cDriverConfig_t;

typedef struct {
    s32_t port;
    u16_t deviceAddress7bit;
    u32_t clockSpeedHz;
    u32_t transactionTimeoutMs;
} I2cDeviceConfig_t;

bool_t I2cDriverInit(const I2cDriverConfig_t *config);
bool_t I2cDriverDeinit(void);

bool_t I2cRegisterDevice(const I2cDeviceConfig_t *config);
bool_t I2cUnregisterDevice(s32_t port, u16_t deviceAddress7bit);
bool_t I2cProbe(s32_t port, u16_t deviceAddress7bit);

/**
 * @brief 7-bit 地址扫描（0x08..0x77），仅在对应总线已通过 I2cDriverInit 初始化后调用。
 * @param on_device 每发现一个应答地址时调用一次；可为 NULL，则只统计数量不回调。
 * @return 应答的设备数量。
 */
typedef void (*I2cScanDeviceCb)(void *user_ctx, u16_t address7bit);
u16_t I2cScanBus7Bit(s32_t port, I2cScanDeviceCb on_device, void *user_ctx);

bool_t I2cWrite(s32_t port, u16_t deviceAddress7bit, const u8_t *writeBuffer, usize_t writeLength);
bool_t I2cRead(s32_t port, u16_t deviceAddress7bit, u8_t *readBuffer, usize_t readLength);
bool_t I2cWriteRead(s32_t port,
                    u16_t deviceAddress7bit,
                    const u8_t *writeBuffer,
                    usize_t writeLength,
                    u8_t *readBuffer,
                    usize_t readLength);
bool_t I2cWriteReg8(s32_t port,
                    u16_t deviceAddress7bit,
                    u8_t registerAddress,
                    const u8_t *writeBuffer,
                    usize_t writeLength);
bool_t I2cReadReg8(s32_t port,
                   u16_t deviceAddress7bit,
                   u8_t registerAddress,
                   u8_t *readBuffer,
                   usize_t readLength);

s32_t I2cGetLastError(void);

#endif
