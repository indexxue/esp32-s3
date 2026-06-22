/**
 * @file lcd_font.h
 * @brief GB2312 + ASCII 混合文本绘制（12/16 点阵，供 ST7789 菜单使用）。
 */

#ifndef LCD_FONT_H
#define LCD_FONT_H

#include <stddef.h>
#include <stdint.h>

#include "st7789.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 估算字符串像素宽度（GB2312 双字节 + ASCII 半宽）。 */
uint16_t lcd_font_text_width(const char *text, uint8_t sizey);

/** 绘制 GB2312/ASCII 混合串；串须为 GB2312 编码（中文）+ ASCII。 */
void lcd_font_draw_text(st7789_t *lcd,
                        uint16_t x,
                        uint16_t y,
                        const char *text,
                        uint16_t fc,
                        uint16_t bc,
                        uint8_t sizey,
                        uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif
