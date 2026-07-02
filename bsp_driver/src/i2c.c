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

typedef struct {
    bool_t busInited;
    i2c_master_bus_handle_t busHandle;
    u32_t defaultClockSpeedHz;
    u32_t defaultTimeoutMs;
    u8_t maxDeviceCount;
    I2cDeviceSlot_t deviceSlots[I2C_DEVICE_SLOTS_MAX];
} I2cPortState_t;

static I2cPortState_t s_i2cPorts[BSP_I2C_HW_PORT_COUNT] = {0};
static esp_err_t s_i2cLastErr = ESP_OK;

static s32_t i2cPortToIndex(s32_t port)
{
    if ((port < 0) || (port >= (s32_t)BSP_I2C_HW_PORT_COUNT)) {
        return -1;
    }
    return port;
}

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

static s32_t i2cFindDeviceSlotByAddress(s32_t portIndex, u16_t address7bit)
{
    u8_t i = 0U;
    u8_t maxCount = s_i2cPorts[portIndex].maxDeviceCount;

    for (i = 0U; i < maxCount; i++) {
        if ((s_i2cPorts[portIndex].deviceSlots[i].inUse == TRUE) &&
            (s_i2cPorts[portIndex].deviceSlots[i].address7bit == address7bit)) {
            return (s32_t)i;
        }
    }
    return -1;
}

static s32_t i2cFindFreeDeviceSlot(s32_t portIndex)
{
    u8_t i = 0U;
    u8_t maxCount = s_i2cPorts[portIndex].maxDeviceCount;

    for (i = 0U; i < maxCount; i++) {
        if (s_i2cPorts[portIndex].deviceSlots[i].inUse == FALSE) {
            return (s32_t)i;
        }
    }
    return -1;
}

static bool_t i2cGetRegisteredDevice(s32_t portIndex,
                                     u16_t address7bit,
                                     i2c_master_dev_handle_t *deviceHandle,
                                     u32_t *timeoutMs)
{
    s32_t slotIndex = -1;
    if ((deviceHandle == NULL) || (timeoutMs == NULL)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_i2cPorts[portIndex].busInited == FALSE) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (i2cCheckAddressValid(address7bit) == FALSE) {
        return FALSE;
    }

    slotIndex = i2cFindDeviceSlotByAddress(portIndex, address7bit);
    if (slotIndex < 0) {
        return i2cSetLastErr(ESP_ERR_NOT_FOUND);
    }

    *deviceHandle = s_i2cPorts[portIndex].deviceSlots[slotIndex].handle;
    *timeoutMs = s_i2cPorts[portIndex].deviceSlots[slotIndex].timeoutMs;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cDriverInit(const I2cDriverConfig_t *config)
{
    i2c_master_bus_config_t busConfig = {0};
    esp_err_t ret = ESP_OK;
    s32_t portIndex = -1;

    if (config == NULL) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    portIndex = i2cPortToIndex(config->port);
    if (portIndex < 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_i2cPorts[portIndex].busInited == TRUE) {
        return i2cSetLastErr(ESP_OK);
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

    ret = i2c_new_master_bus(&busConfig, &s_i2cPorts[portIndex].busHandle);
    if (ret != ESP_OK) {
        return i2cSetLastErr(ret);
    }

    s_i2cPorts[portIndex].defaultClockSpeedHz = config->defaultClockSpeedHz;
    s_i2cPorts[portIndex].defaultTimeoutMs = config->defaultTransactionTimeoutMs;
    s_i2cPorts[portIndex].maxDeviceCount = config->maxDeviceCount;
    s_i2cPorts[portIndex].busInited = TRUE;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cDriverDeinit(void)
{
    esp_err_t ret = ESP_OK;
    s32_t p = 0;
    u8_t i = 0U;

    for (p = 0; p < (s32_t)BSP_I2C_HW_PORT_COUNT; p++) {
        if (s_i2cPorts[p].busInited == FALSE) {
            continue;
        }

        for (i = 0U; i < s_i2cPorts[p].maxDeviceCount; i++) {
            if (s_i2cPorts[p].deviceSlots[i].inUse == TRUE) {
                ret = i2c_master_bus_rm_device(s_i2cPorts[p].deviceSlots[i].handle);
                if (ret != ESP_OK) {
                    return i2cSetLastErr(ret);
                }
                s_i2cPorts[p].deviceSlots[i].inUse = FALSE;
                s_i2cPorts[p].deviceSlots[i].address7bit = 0U;
                s_i2cPorts[p].deviceSlots[i].timeoutMs = 0U;
                s_i2cPorts[p].deviceSlots[i].handle = NULL;
            }
        }

        ret = i2c_del_master_bus(s_i2cPorts[p].busHandle);
        if (ret != ESP_OK) {
            return i2cSetLastErr(ret);
        }

        s_i2cPorts[p].busHandle = NULL;
        s_i2cPorts[p].busInited = FALSE;
        s_i2cPorts[p].defaultClockSpeedHz = 100000U;
        s_i2cPorts[p].defaultTimeoutMs = 1000U;
        s_i2cPorts[p].maxDeviceCount = 0U;
    }

    return i2cSetLastErr(ESP_OK);
}

bool_t I2cRegisterDevice(const I2cDeviceConfig_t *config)
{
    i2c_device_config_t deviceConfig = {0};
    i2c_master_dev_handle_t deviceHandle = NULL;
    s32_t portIndex = -1;
    s32_t slotIndex = -1;
    u32_t clockSpeedHz = 0U;
    u32_t timeoutMs = 0U;
    esp_err_t ret = ESP_OK;

    if (config == NULL) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    portIndex = i2cPortToIndex(config->port);
    if (portIndex < 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_i2cPorts[portIndex].busInited == FALSE) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (i2cCheckAddressValid(config->deviceAddress7bit) == FALSE) {
        return FALSE;
    }
    if (i2cFindDeviceSlotByAddress(portIndex, config->deviceAddress7bit) >= 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }

    slotIndex = i2cFindFreeDeviceSlot(portIndex);
    if (slotIndex < 0) {
        return i2cSetLastErr(ESP_ERR_NO_MEM);
    }

    clockSpeedHz = (config->clockSpeedHz == 0U) ? s_i2cPorts[portIndex].defaultClockSpeedHz
                                                : config->clockSpeedHz;
    timeoutMs = (config->transactionTimeoutMs == 0U) ? s_i2cPorts[portIndex].defaultTimeoutMs
                                                     : config->transactionTimeoutMs;

    deviceConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    deviceConfig.device_address = config->deviceAddress7bit;
    deviceConfig.scl_speed_hz = clockSpeedHz;

    ret = i2c_master_bus_add_device(s_i2cPorts[portIndex].busHandle, &deviceConfig, &deviceHandle);
    if (ret != ESP_OK) {
        return i2cSetLastErr(ret);
    }

    s_i2cPorts[portIndex].deviceSlots[slotIndex].inUse = TRUE;
    s_i2cPorts[portIndex].deviceSlots[slotIndex].address7bit = config->deviceAddress7bit;
    s_i2cPorts[portIndex].deviceSlots[slotIndex].timeoutMs = timeoutMs;
    s_i2cPorts[portIndex].deviceSlots[slotIndex].handle = deviceHandle;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cUnregisterDevice(s32_t port, u16_t deviceAddress7bit)
{
    s32_t portIndex = -1;
    s32_t slotIndex = -1;
    esp_err_t ret = ESP_OK;

    portIndex = i2cPortToIndex(port);
    if (portIndex < 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_i2cPorts[portIndex].busInited == FALSE) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (i2cCheckAddressValid(deviceAddress7bit) == FALSE) {
        return FALSE;
    }

    slotIndex = i2cFindDeviceSlotByAddress(portIndex, deviceAddress7bit);
    if (slotIndex < 0) {
        return i2cSetLastErr(ESP_ERR_NOT_FOUND);
    }

    ret = i2c_master_bus_rm_device(s_i2cPorts[portIndex].deviceSlots[slotIndex].handle);
    if (ret != ESP_OK) {
        return i2cSetLastErr(ret);
    }

    s_i2cPorts[portIndex].deviceSlots[slotIndex].inUse = FALSE;
    s_i2cPorts[portIndex].deviceSlots[slotIndex].address7bit = 0U;
    s_i2cPorts[portIndex].deviceSlots[slotIndex].timeoutMs = 0U;
    s_i2cPorts[portIndex].deviceSlots[slotIndex].handle = NULL;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cGetMasterBusHandle(s32_t port, i2c_master_bus_handle_t *bus_handle)
{
    s32_t portIndex = i2cPortToIndex(port);

    if (bus_handle == NULL) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (portIndex < 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_i2cPorts[portIndex].busInited == FALSE) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }

    *bus_handle = s_i2cPorts[portIndex].busHandle;
    return i2cSetLastErr(ESP_OK);
}

bool_t I2cProbe(s32_t port, u16_t deviceAddress7bit)
{
    esp_err_t ret = ESP_OK;
    s32_t portIndex = -1;

    portIndex = i2cPortToIndex(port);
    if (portIndex < 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_i2cPorts[portIndex].busInited == FALSE) {
        return i2cSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (i2cCheckAddressValid(deviceAddress7bit) == FALSE) {
        return FALSE;
    }

    ret = i2c_master_probe(s_i2cPorts[portIndex].busHandle,
                           deviceAddress7bit,
                           s_i2cPorts[portIndex].defaultTimeoutMs);
    return i2cSetLastErr(ret);
}

u16_t I2cScanBus7Bit(s32_t port, I2cScanDeviceCb on_device, void *user_ctx)
{
    u16_t addr = 0U;
    u16_t foundCount = 0U;
    s32_t portIndex = i2cPortToIndex(port);

    if (portIndex < 0) {
        (void)i2cSetLastErr(ESP_ERR_INVALID_ARG);
        return 0U;
    }
    if (s_i2cPorts[portIndex].busInited == FALSE) {
        (void)i2cSetLastErr(ESP_ERR_INVALID_STATE);
        return 0U;
    }

    for (addr = 0x08U; addr <= 0x77U; addr++) {
        if (I2cProbe(port, addr) == TRUE) {
            foundCount++;
            if (on_device != NULL) {
                on_device(user_ctx, addr);
            }
        }
    }

    return foundCount;
}

bool_t I2cWrite(s32_t port, u16_t deviceAddress7bit, const u8_t *writeBuffer, usize_t writeLength)
{
    i2c_master_dev_handle_t deviceHandle = NULL;
    u32_t timeoutMs = 0U;
    esp_err_t ret = ESP_OK;
    s32_t portIndex = -1;

    if ((writeBuffer == NULL) || (writeLength == 0U)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    portIndex = i2cPortToIndex(port);
    if (portIndex < 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (i2cGetRegisteredDevice(portIndex, deviceAddress7bit, &deviceHandle, &timeoutMs) == FALSE) {
        return FALSE;
    }

    ret = i2c_master_transmit(deviceHandle, writeBuffer, writeLength, timeoutMs);
    return i2cSetLastErr(ret);
}

bool_t I2cRead(s32_t port, u16_t deviceAddress7bit, u8_t *readBuffer, usize_t readLength)
{
    i2c_master_dev_handle_t deviceHandle = NULL;
    u32_t timeoutMs = 0U;
    esp_err_t ret = ESP_OK;
    s32_t portIndex = -1;

    if ((readBuffer == NULL) || (readLength == 0U)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    portIndex = i2cPortToIndex(port);
    if (portIndex < 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (i2cGetRegisteredDevice(portIndex, deviceAddress7bit, &deviceHandle, &timeoutMs) == FALSE) {
        return FALSE;
    }

    ret = i2c_master_receive(deviceHandle, readBuffer, readLength, timeoutMs);
    return i2cSetLastErr(ret);
}

bool_t I2cWriteRead(s32_t port,
                    u16_t deviceAddress7bit,
                    const u8_t *writeBuffer,
                    usize_t writeLength,
                    u8_t *readBuffer,
                    usize_t readLength)
{
    i2c_master_dev_handle_t deviceHandle = NULL;
    u32_t timeoutMs = 0U;
    esp_err_t ret = ESP_OK;
    s32_t portIndex = -1;

    if ((writeBuffer == NULL) || (writeLength == 0U) || (readBuffer == NULL) || (readLength == 0U)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    portIndex = i2cPortToIndex(port);
    if (portIndex < 0) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (i2cGetRegisteredDevice(portIndex, deviceAddress7bit, &deviceHandle, &timeoutMs) == FALSE) {
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

bool_t I2cWriteReg8(s32_t port,
                    u16_t deviceAddress7bit,
                    u8_t registerAddress,
                    const u8_t *writeBuffer,
                    usize_t writeLength)
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

    return I2cWrite(port, deviceAddress7bit, txBuffer, writeLength + 1U);
}

bool_t I2cReadReg8(s32_t port,
                   u16_t deviceAddress7bit,
                   u8_t registerAddress,
                   u8_t *readBuffer,
                   usize_t readLength)
{
    if ((readBuffer == NULL) || (readLength == 0U)) {
        return i2cSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return I2cWriteRead(port, deviceAddress7bit, &registerAddress, 1U, readBuffer, readLength);
}

s32_t I2cGetLastError(void)
{
    return (s32_t)s_i2cLastErr;
}
