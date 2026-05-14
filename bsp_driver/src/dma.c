#include "dma.h"

#include <stddef.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"

bool_t DmaSpiBusChannelIsValid(DmaSpiBusChannel_t channel)
{
    switch (channel) {
    case DMA_SPI_BUS_DISABLED_E:
    case DMA_SPI_BUS_AUTO_E:
        return TRUE;
    default:
        return FALSE;
    }
}

bool_t DmaSdmmcPathIsValid(DmaSdmmcPath_t path)
{
    switch (path) {
    case DMA_SDMMC_PATH_CONTROLLER_IDMAC_E:
        return TRUE;
    default:
        return FALSE;
    }
}

s32_t DmaSpiBusChannelToSpiBusInitValue(DmaSpiBusChannel_t channel)
{
    switch (channel) {
    case DMA_SPI_BUS_DISABLED_E:
        return (s32_t)SPI_DMA_DISABLED;
    case DMA_SPI_BUS_AUTO_E:
        return (s32_t)SPI_DMA_CH_AUTO;
    default:
        return -1;
    }
}

bool_t DmaBufferIsBusCapable(const void *buffer, usize_t length)
{
    const u8_t *p = (const u8_t *)buffer;

    if ((buffer == NULL_PTR) || (length == 0U)) {
        return FALSE;
    }
    if (esp_ptr_dma_capable((void_t *)p) == 0) {
        return FALSE;
    }
    if ((length > 1U) && (esp_ptr_dma_capable((void_t *)(p + length - 1U)) == 0)) {
        return FALSE;
    }
    return TRUE;
}

void *DmaMalloc(usize_t size)
{
    if (size == 0U) {
        return NULL_PTR;
    }
    return heap_caps_malloc((size_t)size, (u32_t)MALLOC_CAP_DMA);
}

void DmaFree(void *ptr)
{
    heap_caps_free(ptr);
}
