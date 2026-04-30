#ifndef DRIVER_I2C_H
#define DRIVER_I2C_H

#include "type.h"

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
    u16_t deviceAddress7bit;
    u32_t clockSpeedHz;
    u32_t transactionTimeoutMs;
} I2cDeviceConfig_t;

bool_t I2cDriverInit(const I2cDriverConfig_t *config);
bool_t I2cDriverDeinit(void);

bool_t I2cRegisterDevice(const I2cDeviceConfig_t *config);
bool_t I2cUnregisterDevice(u16_t deviceAddress7bit);
bool_t I2cProbe(u16_t deviceAddress7bit);
bool_t I2cWrite(u16_t deviceAddress7bit, const u8_t *writeBuffer, usize_t writeLength);
bool_t I2cRead(u16_t deviceAddress7bit, u8_t *readBuffer, usize_t readLength);
bool_t I2cWriteRead(u16_t deviceAddress7bit,
                    const u8_t *writeBuffer,
                    usize_t writeLength,
                    u8_t *readBuffer,
                    usize_t readLength);
bool_t I2cWriteReg8(u16_t deviceAddress7bit, u8_t registerAddress, const u8_t *writeBuffer, usize_t writeLength);
bool_t I2cReadReg8(u16_t deviceAddress7bit, u8_t registerAddress, u8_t *readBuffer, usize_t readLength);

s32_t I2cGetLastError(void);

#endif
