/**
 * @file    st7789.c
 * @brief   ST7789 寄存器与写显存；初始化序列参考 `tmp/LCD/lcd_init.c`（COLMOD 0x05、MADCTL 分方向等）。
 *
 * @note   在 ESP-IDF SPI Master 上为设备配置 `spics_io_num = -1`，由 `set_cs` 软件片选，
 *         以便 `RAMWR` 后可多次 `spi_tx` 而 CS 保持有效。
 */

#include "st7789.h"
#include <stddef.h>
#include <stdint.h>

#define ST7789_CMD_SWRESET 0x01U
#define ST7789_CMD_SLPOUT 0x11U
#define ST7789_CMD_INVON 0x21U
#define ST7789_CMD_DISPON 0x29U
#define ST7789_CMD_CASET 0x2AU
#define ST7789_CMD_RASET 0x2BU
#define ST7789_CMD_RAMWR 0x2CU
#define ST7789_CMD_MADCTL 0x36U
#define ST7789_CMD_COLMOD 0x3AU
#define ST7789_CMD_PORCTRL 0xB2U
#define ST7789_CMD_GCTRL 0xB7U
#define ST7789_CMD_VCOMS 0xBBU
#define ST7789_CMD_LCMCTRL 0xC0U
#define ST7789_CMD_VDVVRHEN 0xC2U
#define ST7789_CMD_VRHS 0xC3U
#define ST7789_CMD_VDVS 0xC4U
#define ST7789_CMD_FRCTRL2 0xC6U
#define ST7789_CMD_PWCTRL1 0xD0U
#define ST7789_CMD_PVGAMCTRL 0xE0U
#define ST7789_CMD_NVGAMCTRL 0xE1U

#define MADCTL_BGR 0x08U

#define TX_CHUNK 4096U

/** 大块像素展开缓冲（内部 SRAM，便于 SPI DMA）；避免在 `st7789_write_pixels` 栈上开大数组。 */
static uint8_t s_rgb565_tx_chunk[TX_CHUNK];

static void dc_cmd(st7789_t *dev) {
    if (dev->set_dc) {
        dev->set_dc(0);
    }
}
static void dc_data(st7789_t *dev) {
    if (dev->set_dc) {
        dev->set_dc(1);
    }
}
static void cs_low(st7789_t *dev) {
    if (dev->set_cs) {
        dev->set_cs(0);
    }
}
static void cs_high(st7789_t *dev) {
    if (dev->set_cs) {
        dev->set_cs(1);
    }
}
static void rst_high(st7789_t *dev) {
    if (dev->set_rst) {
        dev->set_rst(1);
    }
}
static void rst_low(st7789_t *dev) {
    if (dev->set_rst) {
        dev->set_rst(0);
    }
}
static void delay_ms(st7789_t *dev, uint32_t ms) {
    if (dev->delay_ms) {
        dev->delay_ms(ms);
    }
}

static void write_cmd(st7789_t *dev, uint8_t cmd) {
    dc_cmd(dev);
    cs_low(dev);
    if (dev->spi_tx) {
        dev->spi_tx(&cmd, 1);
    }
    cs_high(dev);
}

static void write_data(st7789_t *dev, uint8_t data) {
    dc_data(dev);
    cs_low(dev);
    if (dev->spi_tx) {
        dev->spi_tx(&data, 1);
    }
    cs_high(dev);
}

static void write_data_buf(st7789_t *dev, const uint8_t *buf, uint16_t len) {
    if (len == 0 || !dev->spi_tx) {
        return;
    }
    dc_data(dev);
    cs_low(dev);
    dev->spi_tx(buf, len);
    cs_high(dev);
}

/** 与参考 `lcd_init.c` LCD_Address_Set 相同的 CASET/RASET 偏移。 */
static void map_window(st7789_t *dev, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t *mx0, uint16_t *my0,
                       uint16_t *mx1, uint16_t *my1) {
    switch (dev->rotation) {
    case ST7789_ROT_PORTRAIT_0:
        *mx0 = (uint16_t)(x0 + 52U);
        *mx1 = (uint16_t)(x1 + 52U);
        *my0 = (uint16_t)(y0 + 40U);
        *my1 = (uint16_t)(y1 + 40U);
        break;
    case ST7789_ROT_PORTRAIT_180:
        *mx0 = (uint16_t)(x0 + 53U);
        *mx1 = (uint16_t)(x1 + 53U);
        *my0 = (uint16_t)(y0 + 40U);
        *my1 = (uint16_t)(y1 + 40U);
        break;
    case ST7789_ROT_LANDSCAPE:
        *mx0 = (uint16_t)(x0 + 40U);
        *mx1 = (uint16_t)(x1 + 40U);
        *my0 = (uint16_t)(y0 + 53U);
        *my1 = (uint16_t)(y1 + 53U);
        break;
    default:
        *mx0 = (uint16_t)(x0 + 40U);
        *mx1 = (uint16_t)(x1 + 40U);
        *my0 = (uint16_t)(y0 + 52U);
        *my1 = (uint16_t)(y1 + 52U);
        break;
    }
}

static uint8_t madctl_for_rotation(uint8_t rot) {
    switch (rot) {
    case ST7789_ROT_PORTRAIT_0:
        return 0x00U;
    case ST7789_ROT_PORTRAIT_180:
        return 0xC0U;
    case ST7789_ROT_LANDSCAPE:
        return 0x70U;
    default:
        return 0xA0U;
    }
}

st7789_status_t st7789_register(st7789_t *dev, const st7789_config_t *cfg) {
    if (dev == NULL || cfg == NULL) {
        return ST7789_ERROR_PARAM;
    }
    if (cfg->spi_tx == NULL) {
        return ST7789_ERROR_PARAM;
    }

    dev->spi_tx = cfg->spi_tx;
    dev->set_cs = cfg->set_cs;
    dev->set_dc = cfg->set_dc;
    dev->set_rst = cfg->set_rst;
    dev->set_bl = cfg->set_bl;
    dev->delay_ms = cfg->delay_ms;
    dev->rotation = (cfg->rotation <= 3U) ? cfg->rotation : (uint8_t)ST7789_ROT_LANDSCAPE;
    dev->initialized = false;

    cs_high(dev);
    dc_data(dev);
    rst_high(dev);
    delay_ms(dev, 10);
    rst_low(dev);
    delay_ms(dev, 20);
    rst_high(dev);
    delay_ms(dev, 120);

    write_cmd(dev, ST7789_CMD_SWRESET);
    delay_ms(dev, 150);

    write_cmd(dev, ST7789_CMD_SLPOUT);
    delay_ms(dev, 120);

    write_cmd(dev, ST7789_CMD_MADCTL);
    write_data(dev, madctl_for_rotation(dev->rotation));

    write_cmd(dev, ST7789_CMD_COLMOD);
    write_data(dev, 0x05U);

    write_cmd(dev, ST7789_CMD_PORCTRL);
    write_data(dev, 0x0CU);
    write_data(dev, 0x0CU);
    write_data(dev, 0x00U);
    write_data(dev, 0x33U);
    write_data(dev, 0x33U);

    write_cmd(dev, ST7789_CMD_GCTRL);
    write_data(dev, 0x35U);

    write_cmd(dev, ST7789_CMD_VCOMS);
    write_data(dev, 0x19U);

    write_cmd(dev, ST7789_CMD_LCMCTRL);
    write_data(dev, 0x2CU);

    write_cmd(dev, ST7789_CMD_VDVVRHEN);
    write_data(dev, 0x01U);

    write_cmd(dev, ST7789_CMD_VRHS);
    write_data(dev, 0x12U);

    write_cmd(dev, ST7789_CMD_VDVS);
    write_data(dev, 0x20U);

    write_cmd(dev, ST7789_CMD_FRCTRL2);
    write_data(dev, 0x0FU);

    write_cmd(dev, ST7789_CMD_PWCTRL1);
    write_data(dev, 0xA4U);
    write_data(dev, 0xA1U);

    write_cmd(dev, ST7789_CMD_PVGAMCTRL);
    write_data(dev, 0xD0U);
    write_data(dev, 0x04U);
    write_data(dev, 0x0DU);
    write_data(dev, 0x11U);
    write_data(dev, 0x13U);
    write_data(dev, 0x2BU);
    write_data(dev, 0x3FU);
    write_data(dev, 0x54U);
    write_data(dev, 0x4CU);
    write_data(dev, 0x18U);
    write_data(dev, 0x0DU);
    write_data(dev, 0x0BU);
    write_data(dev, 0x1FU);
    write_data(dev, 0x23U);

    write_cmd(dev, ST7789_CMD_NVGAMCTRL);
    write_data(dev, 0xD0U);
    write_data(dev, 0x04U);
    write_data(dev, 0x0CU);
    write_data(dev, 0x11U);
    write_data(dev, 0x13U);
    write_data(dev, 0x2CU);
    write_data(dev, 0x3FU);
    write_data(dev, 0x44U);
    write_data(dev, 0x51U);
    write_data(dev, 0x2FU);
    write_data(dev, 0x1FU);
    write_data(dev, 0x1FU);
    write_data(dev, 0x20U);
    write_data(dev, 0x23U);

    write_cmd(dev, ST7789_CMD_INVON);
    delay_ms(dev, 10);

    write_cmd(dev, ST7789_CMD_DISPON);
    delay_ms(dev, 20);

    (void)st7789_set_backlight(dev, true);
    dev->initialized = true;
    return ST7789_OK;
}

st7789_status_t st7789_set_backlight(st7789_t *dev, bool on) {
    if (dev == NULL) {
        return ST7789_ERROR_PARAM;
    }
    if (dev->set_bl) {
        dev->set_bl(on ? 1 : 0);
    }
    return ST7789_OK;
}

st7789_status_t st7789_set_window(st7789_t *dev, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    if (dev == NULL || !dev->initialized) {
        return ST7789_ERROR_NOT_INIT;
    }

    uint16_t mx0, my0, mx1, my1;
    map_window(dev, x0, y0, x1, y1, &mx0, &my0, &mx1, &my1);

    uint8_t buf[4];
    write_cmd(dev, ST7789_CMD_CASET);
    buf[0] = (uint8_t)(mx0 >> 8);
    buf[1] = (uint8_t)(mx0 & 0xFFU);
    buf[2] = (uint8_t)(mx1 >> 8);
    buf[3] = (uint8_t)(mx1 & 0xFFU);
    write_data_buf(dev, buf, 4);
    write_cmd(dev, ST7789_CMD_RASET);
    buf[0] = (uint8_t)(my0 >> 8);
    buf[1] = (uint8_t)(my0 & 0xFFU);
    buf[2] = (uint8_t)(my1 >> 8);
    buf[3] = (uint8_t)(my1 & 0xFFU);
    write_data_buf(dev, buf, 4);
    {
        uint8_t ramwr = (uint8_t)ST7789_CMD_RAMWR;
        dc_cmd(dev);
        cs_low(dev);
        if (dev->spi_tx) {
            dev->spi_tx(&ramwr, 1);
        }
    }
    return ST7789_OK;
}

st7789_status_t st7789_write_pixels(st7789_t *dev, const uint16_t *buf, uint32_t len) {
    if (dev == NULL || !dev->initialized) {
        return ST7789_ERROR_NOT_INIT;
    }
    if (buf == NULL || len == 0 || !dev->spi_tx) {
        return ST7789_ERROR_PARAM;
    }

    dc_data(dev);
    /* 与参考 `LCD_WR_DATA` 一致：每像素先高字节再低字节（RGB565 大端在总线上）。 */
    uint32_t idx = 0;
    while (idx < len) {
        uint32_t n = len - idx;
        uint32_t max_pairs = (uint32_t)(sizeof(s_rgb565_tx_chunk) / 2U);
        if (n > max_pairs) {
            n = max_pairs;
        }
        for (uint32_t k = 0; k < n; k++) {
            uint16_t v = buf[idx + k];
            s_rgb565_tx_chunk[k * 2U] = (uint8_t)(v >> 8);
            s_rgb565_tx_chunk[k * 2U + 1U] = (uint8_t)(v & 0xFFU);
        }
        dev->spi_tx(s_rgb565_tx_chunk, (uint16_t)(n * 2U));
        idx += n;
    }
    return ST7789_OK;
}

st7789_status_t st7789_write_pixel_bytes(st7789_t *dev, const uint8_t *buf, uint32_t nbytes) {
    const uint8_t *p;

    if (dev == NULL || !dev->initialized || buf == NULL || nbytes == 0U || dev->spi_tx == NULL) {
        return ST7789_ERROR_PARAM;
    }

    p = buf;
    dc_data(dev);
    while (nbytes > 0U) {
        uint32_t chunk = nbytes;
        if (chunk > 65535U) {
            chunk = 65535U;
        }
        dev->spi_tx(p, (uint16_t)chunk);
        p += chunk;
        nbytes -= chunk;
    }
    return ST7789_OK;
}

void st7789_end_write(st7789_t *dev) {
    if (dev != NULL && dev->set_cs) {
        dev->set_cs(1);
    }
}

bool st7789_is_initialized(const st7789_t *dev) {
    return dev != NULL && dev->initialized;
}

uint16_t st7789_display_width(const st7789_t *dev) {
    if (dev == NULL || !dev->initialized) {
        return 0U;
    }
    return (dev->rotation == ST7789_ROT_PORTRAIT_0 || dev->rotation == ST7789_ROT_PORTRAIT_180) ? 135U : 240U;
}

uint16_t st7789_display_height(const st7789_t *dev) {
    if (dev == NULL || !dev->initialized) {
        return 0U;
    }
    return (dev->rotation == ST7789_ROT_PORTRAIT_0 || dev->rotation == ST7789_ROT_PORTRAIT_180) ? 240U : 135U;
}

st7789_rotation_t st7789_get_rotation(const st7789_t *dev) {
    if (dev == NULL) {
        return ST7789_ROT_LANDSCAPE;
    }
    return (st7789_rotation_t)dev->rotation;
}
