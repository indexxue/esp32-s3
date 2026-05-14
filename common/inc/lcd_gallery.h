/**
 * @file lcd_gallery.h
 * @brief SD 卡全屏图库：扫描 `.bin`（RGB565）与 `.bmp`（BI_RGB 24/32 位），切换与显示。
 */
#ifndef COMMON_LCD_GALLERY_H
#define COMMON_LCD_GALLERY_H

#include "type.h"

#include "st7789.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 扫描挂载点下 `.bin` / `.bmp`（不区分大小写），按路径名排序；无卡或未挂载时清空列表。 */
void lcd_gallery_rescan(void);

/** 当前列表条目数（可为 0）。 */
uint8_t lcd_gallery_count(void);

/** 显示第 `idx` 张（对 `lcd_gallery_count()` 取模）。 */
status_t lcd_gallery_show_index(st7789_t *lcd, uint8_t idx);

/** 切到下一张（循环）。 */
void lcd_gallery_next(void);

/** 当前索引（0 .. count-1；count 为 0 时为 0）。 */
uint8_t lcd_gallery_current(void);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_LCD_GALLERY_H */
