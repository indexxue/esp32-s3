#include "sdio.h"

#include <stddef.h>

#include "dma.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

static esp_err_t s_sdioLastErr = ESP_OK;

static bool_t sdioSetLastErr(esp_err_t err)
{
    s_sdioLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

#if !SOC_SDMMC_HOST_SUPPORTED
bool_t SdioFillSdmmcForBoard(sdmmc_host_t *host, sdmmc_slot_config_t *slot, const SdioBoardConfig_t *cfg)
{
    (void)host;
    (void)slot;
    (void)cfg;
    return sdioSetLastErr(ESP_ERR_NOT_SUPPORTED);
}

s32_t SdioGetLastError(void)
{
    return (s32_t)s_sdioLastErr;
}
#else

bool_t SdioFillSdmmcForBoard(sdmmc_host_t *host, sdmmc_slot_config_t *slot, const SdioBoardConfig_t *cfg)
{
    if ((host == NULL) || (slot == NULL) || (cfg == NULL)) {
        return sdioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if ((cfg->pin_clk < 0) || (cfg->pin_cmd < 0) || (cfg->pin_d0 < 0)) {
        return sdioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if ((cfg->bus_width != 1U) && (cfg->bus_width != 4U)) {
        return sdioSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (cfg->bus_width == 4U) {
        if ((cfg->pin_d1 < 0) || (cfg->pin_d2 < 0) || (cfg->pin_d3 < 0)) {
            return sdioSetLastErr(ESP_ERR_INVALID_ARG);
        }
    }
    if (DmaSdmmcPathIsValid(cfg->sdmmc_dma_path) == FALSE) {
        return sdioSetLastErr(ESP_ERR_INVALID_ARG);
    }

    /* IDF 5.5+：宏展开为 `{ ... }`，须写成复合字面量才能对 `*out` 赋值。 */
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->flags |= (uint32_t)cfg->host_flags_or;
    if (cfg->max_freq_khz != 0U) {
        host->max_freq_khz = (int)cfg->max_freq_khz;
    }
    if (cfg->bus_width == 1U) {
        host->driver_strength = SDMMC_DRIVER_STRENGTH_A;
    }

    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->clk = (gpio_num_t)cfg->pin_clk;
    slot->cmd = (gpio_num_t)cfg->pin_cmd;
    slot->d0  = (gpio_num_t)cfg->pin_d0;
    if (cfg->bus_width == 4U) {
        slot->d1    = (gpio_num_t)cfg->pin_d1;
        slot->d2    = (gpio_num_t)cfg->pin_d2;
        slot->d3    = (gpio_num_t)cfg->pin_d3;
        slot->width = 4;
    } else {
        slot->width = 1;
    }

#if defined(SDMMC_SLOT_FLAG_INTERNAL_PULLUP)
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    return sdioSetLastErr(ESP_OK);
}

s32_t SdioGetLastError(void)
{
    return (s32_t)s_sdioLastErr;
}

#endif /* SOC_SDMMC_HOST_SUPPORTED */
