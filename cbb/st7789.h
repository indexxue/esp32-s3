/**
 * @file    st7789.h
 * @brief   ST7789 TFT：回调 SPI/引脚，与 `tmp/LCD` 参考工程的初始化与坐标偏移（CASET/RASET）对齐。
 */

#ifndef __ST7789_H
#define __ST7789_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 物理面板常见 240x135；逻辑宽高由 `st7789_display_width/height` 随 rotation 变化。 */
#define ST7789_PANEL_W 240U
#define ST7789_PANEL_H 135U

/**
 * 显示方向，与参考 `lcd_init.h` 中 `USE_HORIZONTAL` 0~3 对应。
 * 0/1：竖屏 135x240；2/3：横屏 240x135。
 */
typedef enum {
    ST7789_ROT_PORTRAIT_0 = 0,
    ST7789_ROT_PORTRAIT_180 = 1,
    ST7789_ROT_LANDSCAPE = 2,
    ST7789_ROT_LANDSCAPE_270 = 3,
} st7789_rotation_t;

typedef void (*st7789_spi_tx_t)(const uint8_t *data, uint16_t len);
typedef void (*st7789_pin_t)(int high);
typedef void (*st7789_delay_ms_t)(uint32_t ms);

typedef enum {
    ST7789_OK = 0,
    ST7789_ERROR_PARAM,
    ST7789_ERROR_NOT_INIT,
} st7789_status_t;

typedef struct {
    st7789_spi_tx_t spi_tx;
    st7789_pin_t set_cs;
    st7789_pin_t set_dc;
    st7789_pin_t set_rst;
    st7789_pin_t set_bl;
    st7789_delay_ms_t delay_ms;
    /** 未设置或 >3 时按 `ST7789_ROT_LANDSCAPE` 处理。 */
    uint8_t rotation;
} st7789_config_t;

typedef struct {
    st7789_spi_tx_t spi_tx;
    st7789_pin_t set_cs;
    st7789_pin_t set_dc;
    st7789_pin_t set_rst;
    st7789_pin_t set_bl;
    st7789_delay_ms_t delay_ms;
    uint8_t rotation;
    bool initialized;
} st7789_t;

st7789_status_t st7789_register(st7789_t *dev, const st7789_config_t *cfg);

st7789_status_t st7789_set_backlight(st7789_t *dev, bool on);
st7789_status_t st7789_set_window(st7789_t *dev, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
st7789_status_t st7789_write_pixels(st7789_t *dev, const uint16_t *buf, uint32_t len);
/**
 * 在已调用 `st7789_set_window` 且 CS 保持有效（RAMWR 后）时，按 RGB565 大端顺序发送原始字节流。
 * 单帧内多次调用时总长勿超过窗口像素数 * 2。
 */
st7789_status_t st7789_write_pixel_bytes(st7789_t *dev, const uint8_t *buf, uint32_t nbytes);
void st7789_end_write(st7789_t *dev);

bool st7789_is_initialized(const st7789_t *dev);

uint16_t st7789_display_width(const st7789_t *dev);
uint16_t st7789_display_height(const st7789_t *dev);
st7789_rotation_t st7789_get_rotation(const st7789_t *dev);

#ifdef __cplusplus
}
#endif

#endif
