#include "spi.h"

#include <stddef.h>

#include "driver/spi_master.h"

#define SPI_DEVICE_SLOTS_MAX (8U)
#define SPI_BUS_COUNT (2U)

typedef struct {
    bool_t inUse;
    s32_t chipSelectPin;
    spi_device_handle_t handle;
} SpiDeviceSlot_t;

typedef struct {
    bool_t inited;
    spi_host_device_t host;
    u8_t maxDeviceCount;
    SpiDeviceSlot_t slots[SPI_DEVICE_SLOTS_MAX];
} SpiBusState_t;

static SpiBusState_t s_spiBuses[SPI_BUS_COUNT] = {0};
static esp_err_t s_spiLastErr = ESP_OK;

static bool_t spiSetLastErr(esp_err_t err)
{
    s_spiLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

static s32_t spiHostToIndex(SpiHost_t host)
{
    if (host == SPI_HOST_2_E) {
        return 0;
    }
    if (host == SPI_HOST_3_E) {
        return 1;
    }
    return -1;
}

static SpiBusState_t *spiGetBusByHost(SpiHost_t host)
{
    s32_t idx = spiHostToIndex(host);

    if (idx < 0) {
        return NULL;
    }
    return &s_spiBuses[idx];
}

static s32_t spiFindDeviceSlotByCsPin(SpiBusState_t *bus, s32_t chipSelectPin)
{
    u8_t i = 0U;

    if (bus == NULL) {
        return -1;
    }
    for (i = 0U; i < bus->maxDeviceCount; i++) {
        if ((bus->slots[i].inUse == TRUE) && (bus->slots[i].chipSelectPin == chipSelectPin)) {
            return (s32_t)i;
        }
    }
    return -1;
}

static s32_t spiFindFreeDeviceSlot(SpiBusState_t *bus)
{
    u8_t i = 0U;

    if (bus == NULL) {
        return -1;
    }
    for (i = 0U; i < bus->maxDeviceCount; i++) {
        if (bus->slots[i].inUse == FALSE) {
            return (s32_t)i;
        }
    }
    return -1;
}

static bool_t spiFindDeviceAcrossBuses(s32_t chipSelectPin, spi_device_handle_t *deviceHandle)
{
    u8_t b = 0U;

    if (deviceHandle == NULL) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }

    for (b = 0U; b < SPI_BUS_COUNT; b++) {
        SpiBusState_t *bus = &s_spiBuses[b];
        s32_t slotIndex;

        if (bus->inited == FALSE) {
            continue;
        }
        slotIndex = spiFindDeviceSlotByCsPin(bus, chipSelectPin);
        if (slotIndex >= 0) {
            *deviceHandle = bus->slots[slotIndex].handle;
            return spiSetLastErr(ESP_OK);
        }
    }
    return spiSetLastErr(ESP_ERR_NOT_FOUND);
}

bool_t SpiDriverInit(const SpiDriverConfig_t *config)
{
    SpiBusState_t *bus = NULL;
    spi_bus_config_t busConfig = {0};
    esp_err_t ret = ESP_OK;
    s32_t dmaInitVal = 0;

    if (config == NULL) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    bus = spiGetBusByHost(config->host);
    if (bus == NULL) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (bus->inited == TRUE) {
        return spiSetLastErr(ESP_OK);
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

    dmaInitVal = DmaSpiBusChannelToSpiBusInitValue(config->dmaChannel);
    if (dmaInitVal < 0) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }

    bus->host = (spi_host_device_t)config->host;
    ret = spi_bus_initialize(bus->host, &busConfig, (spi_dma_chan_t)dmaInitVal);
    if (ret != ESP_OK) {
        return spiSetLastErr(ret);
    }

    bus->maxDeviceCount = config->maxDeviceCount;
    bus->inited = TRUE;
    return spiSetLastErr(ESP_OK);
}

bool_t SpiDriverDeinitHost(SpiHost_t host)
{
    SpiBusState_t *bus = spiGetBusByHost(host);
    esp_err_t ret = ESP_OK;
    u8_t i = 0U;

    if (bus == NULL) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (bus->inited == FALSE) {
        return spiSetLastErr(ESP_OK);
    }

    for (i = 0U; i < bus->maxDeviceCount; i++) {
        if (bus->slots[i].inUse == TRUE) {
            ret = spi_bus_remove_device(bus->slots[i].handle);
            if (ret != ESP_OK) {
                return spiSetLastErr(ret);
            }
            bus->slots[i].inUse = FALSE;
            bus->slots[i].chipSelectPin = -1;
            bus->slots[i].handle = NULL;
        }
    }

    ret = spi_bus_free(bus->host);
    if (ret != ESP_OK) {
        return spiSetLastErr(ret);
    }

    bus->maxDeviceCount = 0U;
    bus->inited = FALSE;
    return spiSetLastErr(ESP_OK);
}

bool_t SpiDriverDeinit(void)
{
    if (SpiDriverDeinitHost(SPI_HOST_2_E) == FALSE) {
        return FALSE;
    }
    return SpiDriverDeinitHost(SPI_HOST_3_E);
}

bool_t SpiRegisterDevice(const SpiDeviceConfig_t *config)
{
    SpiBusState_t *bus = NULL;
    spi_device_interface_config_t devConfig = {0};
    spi_device_handle_t devHandle = NULL;
    s32_t slotIndex = -1;
    esp_err_t ret = ESP_OK;
    u8_t b = 0U;

    if (config == NULL) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (config->clockSpeedHz == 0U) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (config->queueSize == 0U) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }

    bus = spiGetBusByHost(config->host);
    if ((bus == NULL) || (bus->inited == FALSE)) {
        return spiSetLastErr(ESP_ERR_INVALID_STATE);
    }

    /* CS 在所有已初始化总线上唯一（含 -1）。 */
    for (b = 0U; b < SPI_BUS_COUNT; b++) {
        if ((s_spiBuses[b].inited == TRUE) &&
            (spiFindDeviceSlotByCsPin(&s_spiBuses[b], config->chipSelectPin) >= 0)) {
            return spiSetLastErr(ESP_ERR_INVALID_STATE);
        }
    }

    slotIndex = spiFindFreeDeviceSlot(bus);
    if (slotIndex < 0) {
        return spiSetLastErr(ESP_ERR_NO_MEM);
    }

    devConfig.spics_io_num = config->chipSelectPin;
    devConfig.clock_speed_hz = (s32_t)config->clockSpeedHz;
    devConfig.mode = (u8_t)config->mode;
    devConfig.flags = config->flags;
    devConfig.queue_size = config->queueSize;
    devConfig.cs_ena_pretrans = config->csEnaPretrans;
    devConfig.cs_ena_posttrans = config->csEnaPosttrans;

    ret = spi_bus_add_device(bus->host, &devConfig, &devHandle);
    if (ret != ESP_OK) {
        return spiSetLastErr(ret);
    }

    bus->slots[slotIndex].inUse = TRUE;
    bus->slots[slotIndex].chipSelectPin = config->chipSelectPin;
    bus->slots[slotIndex].handle = devHandle;
    return spiSetLastErr(ESP_OK);
}

bool_t SpiUnregisterDevice(s32_t chipSelectPin)
{
    u8_t b = 0U;
    esp_err_t ret = ESP_OK;

    for (b = 0U; b < SPI_BUS_COUNT; b++) {
        SpiBusState_t *bus = &s_spiBuses[b];
        s32_t slotIndex;

        if (bus->inited == FALSE) {
            continue;
        }
        slotIndex = spiFindDeviceSlotByCsPin(bus, chipSelectPin);
        if (slotIndex < 0) {
            continue;
        }

        ret = spi_bus_remove_device(bus->slots[slotIndex].handle);
        if (ret != ESP_OK) {
            return spiSetLastErr(ret);
        }
        bus->slots[slotIndex].inUse = FALSE;
        bus->slots[slotIndex].chipSelectPin = -1;
        bus->slots[slotIndex].handle = NULL;
        return spiSetLastErr(ESP_OK);
    }
    return spiSetLastErr(ESP_ERR_NOT_FOUND);
}

bool_t SpiTransmit(s32_t chipSelectPin, const u8_t *txBuffer, usize_t txLength)
{
    spi_device_handle_t devHandle = NULL;
    spi_transaction_t trans = {0};
    esp_err_t ret = ESP_OK;

    if ((txBuffer == NULL) || (txLength == 0U)) {
        return spiSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (spiFindDeviceAcrossBuses(chipSelectPin, &devHandle) == FALSE) {
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
    if (spiFindDeviceAcrossBuses(chipSelectPin, &devHandle) == FALSE) {
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
    if (spiFindDeviceAcrossBuses(chipSelectPin, &devHandle) == FALSE) {
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
