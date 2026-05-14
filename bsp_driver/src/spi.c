#include "spi.h"

#include <stddef.h>

#include "driver/spi_master.h"

#define SPI_DEVICE_SLOTS_MAX (8U)

typedef struct {
    bool_t inUse;
    s32_t chipSelectPin;
    spi_device_handle_t handle;
} SpiDeviceSlot_t;

static bool_t s_spiDriverInited = FALSE;
static spi_host_device_t s_spiHost = SPI2_HOST;
static u8_t s_spiMaxDeviceCount = 0U;
static SpiDeviceSlot_t s_spiDeviceSlots[SPI_DEVICE_SLOTS_MAX] = {0};
static esp_err_t s_spiLastErr = ESP_OK;

static bool_t spiSetLastErr(esp_err_t err)
{
    s_spiLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

static s32_t spiFindDeviceSlotByCsPin(s32_t chipSelectPin)
{
    u8_t i = 0U;
    for (i = 0U; i < s_spiMaxDeviceCount; i++) {
        if ((s_spiDeviceSlots[i].inUse == TRUE) && (s_spiDeviceSlots[i].chipSelectPin == chipSelectPin)) {
            return (s32_t)i;
        }
    }

    return -1;
}

static s32_t spiFindFreeDeviceSlot(void)
{
    u8_t i = 0U;
    for (i = 0U; i < s_spiMaxDeviceCount; i++) {
        if (s_spiDeviceSlots[i].inUse == FALSE) {
            return (s32_t)i;
        }
    }

    return -1;
}

static bool_t spiGetRegisteredDevice(s32_t chipSelectPin, spi_device_handle_t *deviceHandle)
{
    s32_t slotIndex = -1;

    if (deviceHandle == NULL) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_spiDriverInited == FALSE) {
        return spiSetLastErr(ESP_ERR_INVALID_STATE);
    }

    slotIndex = spiFindDeviceSlotByCsPin(chipSelectPin);
    if (slotIndex < 0) {
        return spiSetLastErr(ESP_ERR_NOT_FOUND);
    }

    *deviceHandle = s_spiDeviceSlots[slotIndex].handle;
    return spiSetLastErr(ESP_OK);
}

bool_t SpiDriverInit(const SpiDriverConfig_t *config)
{
    spi_bus_config_t busConfig = {0};
    esp_err_t ret = ESP_OK;

    if (s_spiDriverInited == TRUE) {
        return spiSetLastErr(ESP_OK);
    }
    if (config == NULL) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if ((config->host != SPI_HOST_2_E) && (config->host != SPI_HOST_3_E)) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if ((config->maxDeviceCount == 0U) || (config->maxDeviceCount > SPI_DEVICE_SLOTS_MAX)) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (DmaSpiBusChannelIsValid(config->dmaChannel) == FALSE) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }

    busConfig.sclk_io_num = config->sclkPin;
    busConfig.mosi_io_num = config->mosiPin;
    busConfig.miso_io_num = config->misoPin;
    busConfig.quadwp_io_num = config->quadWpPin;
    busConfig.quadhd_io_num = config->quadHdPin;
    busConfig.max_transfer_sz = config->maxTransferSize;
    busConfig.intr_flags = config->intrFlags;

    s_spiHost = (spi_host_device_t)config->host;
    {
        s32_t dmaInitVal = DmaSpiBusChannelToSpiBusInitValue(config->dmaChannel);
        if (dmaInitVal < 0) {
            return spiSetLastErr(ESP_ERR_INVALID_ARG);
        }
        ret = spi_bus_initialize(s_spiHost, &busConfig, (spi_dma_chan_t)dmaInitVal);
    }
    if (ret != ESP_OK) {
        return spiSetLastErr(ret);
    }

    s_spiMaxDeviceCount = config->maxDeviceCount;
    s_spiDriverInited = TRUE;
    return spiSetLastErr(ESP_OK);
}

bool_t SpiDriverDeinit(void)
{
    esp_err_t ret = ESP_OK;
    u8_t i = 0U;

    if (s_spiDriverInited == FALSE) {
        return spiSetLastErr(ESP_OK);
    }

    for (i = 0U; i < s_spiMaxDeviceCount; i++) {
        if (s_spiDeviceSlots[i].inUse == TRUE) {
            ret = spi_bus_remove_device(s_spiDeviceSlots[i].handle);
            if (ret != ESP_OK) {
                return spiSetLastErr(ret);
            }
            s_spiDeviceSlots[i].inUse = FALSE;
            s_spiDeviceSlots[i].chipSelectPin = -1;
            s_spiDeviceSlots[i].handle = NULL;
        }
    }

    ret = spi_bus_free(s_spiHost);
    if (ret != ESP_OK) {
        return spiSetLastErr(ret);
    }

    s_spiHost = SPI2_HOST;
    s_spiMaxDeviceCount = 0U;
    s_spiDriverInited = FALSE;
    return spiSetLastErr(ESP_OK);
}

bool_t SpiRegisterDevice(const SpiDeviceConfig_t *config)
{
    spi_device_interface_config_t devConfig = {0};
    spi_device_handle_t devHandle = NULL;
    s32_t slotIndex = -1;
    esp_err_t ret = ESP_OK;

    if (s_spiDriverInited == FALSE) {
        return spiSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (config == NULL) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (config->clockSpeedHz == 0U) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (config->queueSize == 0U) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    /* chipSelectPin 可为 -1：SPI 外设不接管 CS，由上层 GPIO 配合 TFT 类连续传输。 */
    if (spiFindDeviceSlotByCsPin(config->chipSelectPin) >= 0) {
        return spiSetLastErr(ESP_ERR_INVALID_STATE);
    }

    slotIndex = spiFindFreeDeviceSlot();
    if (slotIndex < 0) {
        return spiSetLastErr(ESP_ERR_NO_MEM);
    }

    devConfig.spics_io_num = config->chipSelectPin;
    devConfig.clock_speed_hz = (s32_t)config->clockSpeedHz;
    devConfig.mode = (u8_t)config->mode;
    devConfig.flags = config->flags;
    devConfig.queue_size = config->queueSize;

    ret = spi_bus_add_device(s_spiHost, &devConfig, &devHandle);
    if (ret != ESP_OK) {
        return spiSetLastErr(ret);
    }

    s_spiDeviceSlots[slotIndex].inUse = TRUE;
    s_spiDeviceSlots[slotIndex].chipSelectPin = config->chipSelectPin;
    s_spiDeviceSlots[slotIndex].handle = devHandle;
    return spiSetLastErr(ESP_OK);
}

bool_t SpiUnregisterDevice(s32_t chipSelectPin)
{
    s32_t slotIndex = -1;
    esp_err_t ret = ESP_OK;

    if (s_spiDriverInited == FALSE) {
        return spiSetLastErr(ESP_ERR_INVALID_STATE);
    }

    slotIndex = spiFindDeviceSlotByCsPin(chipSelectPin);
    if (slotIndex < 0) {
        return spiSetLastErr(ESP_ERR_NOT_FOUND);
    }

    ret = spi_bus_remove_device(s_spiDeviceSlots[slotIndex].handle);
    if (ret != ESP_OK) {
        return spiSetLastErr(ret);
    }

    s_spiDeviceSlots[slotIndex].inUse = FALSE;
    s_spiDeviceSlots[slotIndex].chipSelectPin = -1;
    s_spiDeviceSlots[slotIndex].handle = NULL;
    return spiSetLastErr(ESP_OK);
}

bool_t SpiTransmit(s32_t chipSelectPin, const u8_t *txBuffer, usize_t txLength)
{
    spi_device_handle_t devHandle = NULL;
    spi_transaction_t trans = {0};
    esp_err_t ret = ESP_OK;

    if ((txBuffer == NULL) || (txLength == 0U)) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (spiGetRegisteredDevice(chipSelectPin, &devHandle) == FALSE) {
        return FALSE;
    }

    trans.tx_buffer = txBuffer;
    trans.length = txLength * 8U;

    ret = spi_device_transmit(devHandle, &trans);
    return spiSetLastErr(ret);
}

bool_t SpiReceive(s32_t chipSelectPin, u8_t *rxBuffer, usize_t rxLength)
{
    spi_device_handle_t devHandle = NULL;
    spi_transaction_t trans = {0};
    esp_err_t ret = ESP_OK;

    if ((rxBuffer == NULL) || (rxLength == 0U)) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (spiGetRegisteredDevice(chipSelectPin, &devHandle) == FALSE) {
        return FALSE;
    }

    trans.rx_buffer = rxBuffer;
    trans.length = rxLength * 8U;
    trans.rxlength = rxLength * 8U;

    ret = spi_device_transmit(devHandle, &trans);
    return spiSetLastErr(ret);
}

bool_t SpiTransmitReceive(s32_t chipSelectPin, const u8_t *txBuffer, u8_t *rxBuffer, usize_t length)
{
    spi_device_handle_t devHandle = NULL;
    spi_transaction_t trans = {0};
    esp_err_t ret = ESP_OK;

    if ((txBuffer == NULL) || (rxBuffer == NULL) || (length == 0U)) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (spiGetRegisteredDevice(chipSelectPin, &devHandle) == FALSE) {
        return FALSE;
    }

    trans.tx_buffer = txBuffer;
    trans.rx_buffer = rxBuffer;
    trans.length = length * 8U;
    trans.rxlength = length * 8U;

    ret = spi_device_transmit(devHandle, &trans);
    return spiSetLastErr(ret);
}

bool_t SpiTransmitDma(s32_t chipSelectPin, const u8_t *txBuffer, usize_t txLength)
{
    if (DmaBufferIsBusCapable(txBuffer, txLength) == FALSE) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return SpiTransmit(chipSelectPin, txBuffer, txLength);
}

bool_t SpiReceiveDma(s32_t chipSelectPin, u8_t *rxBuffer, usize_t rxLength)
{
    if (DmaBufferIsBusCapable(rxBuffer, rxLength) == FALSE) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return SpiReceive(chipSelectPin, rxBuffer, rxLength);
}

bool_t SpiTransmitReceiveDma(s32_t chipSelectPin, const u8_t *txBuffer, u8_t *rxBuffer, usize_t length)
{
    if (DmaBufferIsBusCapable(txBuffer, length) == FALSE) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (DmaBufferIsBusCapable(rxBuffer, length) == FALSE) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return SpiTransmitReceive(chipSelectPin, txBuffer, rxBuffer, length);
}

s32_t SpiGetLastError(void)
{
    return (s32_t)s_spiLastErr;
}
