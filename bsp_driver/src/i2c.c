#include "i2c.h"

#include <stddef.h>

#include "driver/i2c_master.h"

#define I2C_DEVICE_SLOTS_MAX (16U)

typedef struct {
    bool_t inUse;
    u16_t address7bit;
    u32_t timeoutMs;
    i2c_master_dev_handle_t handle;
} I2cDeviceSlot_t;

static bool_t s_i2cDriverInited = FALSE;
static i2c_master_bus_handle_t s_i2cBusHandle = NULL;
static u32_t s_i2cDefaultClockSpeedHz = 100000U;
static u32_t s_i2cDefaultTimeoutMs = 1000U;
static u8_t s_i2cMaxDeviceCount = 0U;
static I2cDeviceSlot_t s_i2cDeviceSlots[I2C_DEVICE_SLOTS_MAX] = {0};
static esp_err_t s_i2cLastErr = ESP_OK;

static bool_t i2cSetLastErr(esp_err_t err)
{
    s_i2cLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

static bool_t i2cCheckAddressValid(u16_t address7bit)
{
    if ((address7bit == 0U) || (address7bit > 0x7FU)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return i2cSetLastErr(ESP_OK);
}

static s32_t i2cFindDeviceSlotByAddress(u16_t address7bit)
{
    u8_t i = 0U;
    for (i = 0U; i < s_i2cMaxDeviceCount; i++) {
        if ((s_i2cDeviceSlots[i].inUse == TRUE) && (s_i2cDeviceSlots[i].address7bit == address7bit)) {
            return (s32_t)i;
        }
    }
    return -1;
}

static s32_t i2cFindFreeDeviceSlot(void)
{
    u8_t i = 0U;
    for (i = 0U; i < s_i2cMaxDeviceCount; i++) {
        if (s_i2cDeviceSlots[i].inUse == FALSE) {
            return (s32_t)i;
        }
    }
    return -1;
}

static bool_t i2cGetRegisteredDevice(u16_t address7bit, i2c_master_dev_handle_t *deviceHandle, u32_t *timeoutMs)
{
    s32_t slotIndex = -1;
    if ((deviceHandle == NULL) || (timeoutMs == NULL)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (!s_i2cDriverInited) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (i2cCheckAddressValid(address7bit) == FALSE) {
        return FALSE;
    }

    slotIndex = i2cFindDeviceSlotByAddress(address7bit);
    if (slotIndex < 0) {
        return i2cSetLastErr(ESP_ERR_NOT_FOUND);
    }

    *deviceHandle = s_i2cDeviceSlots[slotIndex].handle;
    *timeoutMs = s_i2cDeviceSlots[slotIndex].timeoutMs;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cDriverInit(const I2cDriverConfig_t *config)
{
    i2c_master_bus_config_t busConfig = {0};
    esp_err_t ret = ESP_OK;

    if (s_i2cDriverInited) {
        return i2cSetLastErr(ESP_OK);
    }
    if (config == NULL) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (config->defaultClockSpeedHz == 0U) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (config->defaultTransactionTimeoutMs == 0U) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if ((config->maxDeviceCount == 0U) || (config->maxDeviceCount > I2C_DEVICE_SLOTS_MAX)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }

    busConfig.i2c_port = (i2c_port_num_t)config->port;
    busConfig.sda_io_num = (gpio_num_t)config->sdaPin;
    busConfig.scl_io_num = (gpio_num_t)config->sclPin;
    busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    busConfig.glitch_ignore_cnt = config->glitchIgnoreCount;
    busConfig.flags.enable_internal_pullup =
        ((config->enableSdaPullup == TRUE) || (config->enableSclPullup == TRUE)) ? 1 : 0;

    ret = i2c_new_master_bus(&busConfig, &s_i2cBusHandle);
    if (ret != ESP_OK) {
        return i2cSetLastErr(ret);
    }

    s_i2cDefaultClockSpeedHz = config->defaultClockSpeedHz;
    s_i2cDefaultTimeoutMs = config->defaultTransactionTimeoutMs;
    s_i2cMaxDeviceCount = config->maxDeviceCount;
    s_i2cDriverInited = TRUE;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cDriverDeinit(void)
{
    esp_err_t ret = ESP_OK;
    u8_t i = 0U;

    if (!s_i2cDriverInited) {
        return i2cSetLastErr(ESP_OK);
    }

    for (i = 0U; i < s_i2cMaxDeviceCount; i++) {
        if (s_i2cDeviceSlots[i].inUse == TRUE) {
            ret = i2c_master_bus_rm_device(s_i2cDeviceSlots[i].handle);
            if (ret != ESP_OK) {
                return i2cSetLastErr(ret);
            }
            s_i2cDeviceSlots[i].inUse = FALSE;
            s_i2cDeviceSlots[i].address7bit = 0U;
            s_i2cDeviceSlots[i].timeoutMs = 0U;
            s_i2cDeviceSlots[i].handle = NULL;
        }
    }

    ret = i2c_del_master_bus(s_i2cBusHandle);
    if (ret != ESP_OK) {
        return i2cSetLastErr(ret);
    }

    s_i2cBusHandle = NULL;
    s_i2cDriverInited = FALSE;
    s_i2cDefaultClockSpeedHz = 100000U;
    s_i2cDefaultTimeoutMs = 1000U;
    s_i2cMaxDeviceCount = 0U;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cRegisterDevice(const I2cDeviceConfig_t *config)
{
    i2c_device_config_t deviceConfig = {0};
    i2c_master_dev_handle_t deviceHandle = NULL;
    s32_t slotIndex = -1;
    u32_t clockSpeedHz = 0U;
    u32_t timeoutMs = 0U;
    esp_err_t ret = ESP_OK;

    if (!s_i2cDriverInited) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (config == NULL) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (i2cCheckAddressValid(config->deviceAddress7bit) == FALSE) {
        return FALSE;
    }
    if (i2cFindDeviceSlotByAddress(config->deviceAddress7bit) >= 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }

    slotIndex = i2cFindFreeDeviceSlot();
    if (slotIndex < 0) {
        return i2cSetLastErr(ESP_ERR_NO_MEM);
    }

    clockSpeedHz = (config->clockSpeedHz == 0U) ? s_i2cDefaultClockSpeedHz : config->clockSpeedHz;
    timeoutMs = (config->transactionTimeoutMs == 0U) ? s_i2cDefaultTimeoutMs : config->transactionTimeoutMs;

    deviceConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    deviceConfig.device_address = config->deviceAddress7bit;
    deviceConfig.scl_speed_hz = clockSpeedHz;

    ret = i2c_master_bus_add_device(s_i2cBusHandle, &deviceConfig, &deviceHandle);
    if (ret != ESP_OK) {
        return i2cSetLastErr(ret);
    }

    s_i2cDeviceSlots[slotIndex].inUse = TRUE;
    s_i2cDeviceSlots[slotIndex].address7bit = config->deviceAddress7bit;
    s_i2cDeviceSlots[slotIndex].timeoutMs = timeoutMs;
    s_i2cDeviceSlots[slotIndex].handle = deviceHandle;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cUnregisterDevice(u16_t deviceAddress7bit)
{
    s32_t slotIndex = -1;
    esp_err_t ret = ESP_OK;

    if (!s_i2cDriverInited) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (i2cCheckAddressValid(deviceAddress7bit) == FALSE) {
        return FALSE;
    }

    slotIndex = i2cFindDeviceSlotByAddress(deviceAddress7bit);
    if (slotIndex < 0) {
        return i2cSetLastErr(ESP_ERR_NOT_FOUND);
    }

    ret = i2c_master_bus_rm_device(s_i2cDeviceSlots[slotIndex].handle);
    if (ret != ESP_OK) {
        return i2cSetLastErr(ret);
    }

    s_i2cDeviceSlots[slotIndex].inUse = FALSE;
    s_i2cDeviceSlots[slotIndex].address7bit = 0U;
    s_i2cDeviceSlots[slotIndex].timeoutMs = 0U;
    s_i2cDeviceSlots[slotIndex].handle = NULL;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cProbe(u16_t deviceAddress7bit)
{
    esp_err_t ret = ESP_OK;

    if (!s_i2cDriverInited) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (i2cCheckAddressValid(deviceAddress7bit) == FALSE) {
        return FALSE;
    }

    ret = i2c_master_probe(s_i2cBusHandle, deviceAddress7bit, s_i2cDefaultTimeoutMs);
    return i2cSetLastErr(ret);
}

bool_t I2cWrite(u16_t deviceAddress7bit, const u8_t *writeBuffer, usize_t writeLength)
{
    i2c_master_dev_handle_t deviceHandle = NULL;
    u32_t timeoutMs = 0U;
    esp_err_t ret = ESP_OK;

    if ((writeBuffer == NULL) || (writeLength == 0U)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (i2cGetRegisteredDevice(deviceAddress7bit, &deviceHandle, &timeoutMs) == FALSE) {
        return FALSE;
    }

    ret = i2c_master_transmit(deviceHandle, writeBuffer, writeLength, timeoutMs);
    return i2cSetLastErr(ret);
}

bool_t I2cRead(u16_t deviceAddress7bit, u8_t *readBuffer, usize_t readLength)
{
    i2c_master_dev_handle_t deviceHandle = NULL;
    u32_t timeoutMs = 0U;
    esp_err_t ret = ESP_OK;

    if ((readBuffer == NULL) || (readLength == 0U)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (i2cGetRegisteredDevice(deviceAddress7bit, &deviceHandle, &timeoutMs) == FALSE) {
        return FALSE;
    }

    ret = i2c_master_receive(deviceHandle, readBuffer, readLength, timeoutMs);
    return i2cSetLastErr(ret);
}

bool_t I2cWriteRead(u16_t deviceAddress7bit,
                    const u8_t *writeBuffer,
                    usize_t writeLength,
                    u8_t *readBuffer,
                    usize_t readLength)
{
    i2c_master_dev_handle_t deviceHandle = NULL;
    u32_t timeoutMs = 0U;
    esp_err_t ret = ESP_OK;

    if ((writeBuffer == NULL) || (writeLength == 0U) || (readBuffer == NULL) || (readLength == 0U)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (i2cGetRegisteredDevice(deviceAddress7bit, &deviceHandle, &timeoutMs) == FALSE) {
        return FALSE;
    }

    ret = i2c_master_transmit_receive(deviceHandle,
                                      writeBuffer,
                                      writeLength,
                                      readBuffer,
                                      readLength,
                                      timeoutMs);
    return i2cSetLastErr(ret);
}

bool_t I2cWriteReg8(u16_t deviceAddress7bit, u8_t registerAddress, const u8_t *writeBuffer, usize_t writeLength)
{
    u8_t txBuffer[257] = {0};
    usize_t i = 0U;

    if ((writeBuffer == NULL) || (writeLength == 0U) || (writeLength > 256U)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }

    txBuffer[0] = registerAddress;
    for (i = 0U; i < writeLength; i++) {
        txBuffer[i + 1U] = writeBuffer[i];
    }

    return I2cWrite(deviceAddress7bit, txBuffer, writeLength + 1U);
}

bool_t I2cReadReg8(u16_t deviceAddress7bit, u8_t registerAddress, u8_t *readBuffer, usize_t readLength)
{
    if ((readBuffer == NULL) || (readLength == 0U)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return I2cWriteRead(deviceAddress7bit, &registerAddress, 1U, readBuffer, readLength);
}

s32_t I2cGetLastError(void)
{
    return (s32_t)s_i2cLastErr;
}
