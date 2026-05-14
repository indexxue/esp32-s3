/**
 * @file    lcd.h
 * @brief   基于 ST7789 的绘图与字库接口（逻辑由 `tmp/LCD/lcd.c` 移植）。
 *
 * 所有坐标为逻辑坐标：左上角 (0,0)，宽高见 `st7789_display_width/height`。
 * 汉字显示函数按字库索引使用 **GB2312** 双字节编码，需与 `lcdfont.h` 中词条一致
 * （源码为 UTF-8 时请先转为 GB2312 或使用十六进制字节串）。
 */

#ifndef CBB_LCD_H
#define CBB_LCD_H

#include <stdint.h>

#include "st7789.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_COLOR_WHITE 0xFFFFU
#define LCD_COLOR_BLACK 0x0000U
#define LCD_COLOR_BLUE 0x001FU
#define LCD_COLOR_BRED 0xF81FU
#define LCD_COLOR_GRED 0xFFE0U
#define LCD_COLOR_GBLUE 0x07FFU
#define LCD_COLOR_RED 0xF800U
#define LCD_COLOR_MAGENTA 0xF81FU
#define LCD_COLOR_GREEN 0x07E0U
#define LCD_COLOR_CYAN 0x7FFFU
#define LCD_COLOR_YELLOW 0xFFE0U
#define LCD_COLOR_BROWN 0xBC40U
#define LCD_COLOR_BRRED 0xFC07U
#define LCD_COLOR_GRAY 0x8430U
#define LCD_COLOR_DARKBLUE 0x01CFU
#define LCD_COLOR_LIGHTBLUE 0x7D7CU
#define LCD_COLOR_GRAYBLUE 0x5458U
#define LCD_COLOR_LIGHTGREEN 0x841FU
#define LCD_COLOR_LGRAY 0xC618U
#define LCD_COLOR_LGRAYBLUE 0xA651U
#define LCD_COLOR_LBBLUE 0x2B12U

void lcd_fill(st7789_t *lcd, uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color);
/** 大块 DMA 友好纯色填充（见 `lcd.c`），矩形宽度勿超过 `LCD_LINEBUF_MAX`。 */
void lcd_fill_fast(st7789_t *lcd, uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color);
void lcd_draw_point(st7789_t *lcd, uint16_t x, uint16_t y, uint16_t color);
void lcd_draw_line(st7789_t *lcd, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_rectangle(st7789_t *lcd, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void lcd_draw_circle(st7789_t *lcd, uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);

void lcd_show_chinese(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void lcd_show_chinese_12x12(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void lcd_show_chinese_16x16(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void lcd_show_chinese_24x24(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void lcd_show_chinese_32x32(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);

void lcd_show_char(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t num, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void lcd_show_string(st7789_t *lcd, uint16_t x, uint16_t y, const uint8_t *p, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
uint32_t lcd_ui_pow(uint8_t m, uint8_t n);
void lcd_show_int_num(st7789_t *lcd, uint16_t x, uint16_t y, uint16_t num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey);
void lcd_show_float_num1(st7789_t *lcd, uint16_t x, uint16_t y, float num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey);

/** `pic`：RGB565 大端；`length`=水平像素，`width`=垂直像素；缓冲区为**行优先**（与 `lcd_rgb565_convert.py` 默认一致）。 */
void lcd_show_picture(st7789_t *lcd, uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t *pic);

#ifdef __cplusplus
}
#endif

#endif
