/**
 * @file lcd_gallery.h
 * @brief SD 卡全屏图库：扫描 `.bin`（RGB565）与 `.bmp`（BI_RGB 24/32 位），切换与显示。
 */
#ifndef COMMON_LCD_GALLERY_H
#define COMMON_LCD_GALLERY_H

#include "type.h"

#include "st7789.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SD 卡图片目录（`BOARD_SDCARD_MOUNT_POINT/picture`）。 */
#define LCD_GALLERY_DIR_SUFFIX "/picture"

/** 返回图库目录绝对路径（内部静态缓冲，非线程安全）。 */
const char *lcd_gallery_dir_path(void);

/** 扫描 `picture/` 下 `.bin` / `.bmp`（不区分大小写），按路径名排序；无卡或未挂载时清空列表。 */
void lcd_gallery_rescan(void);

/** 当前列表条目数（可为 0）。 */
uint8_t lcd_gallery_count(void);

/** 显示第 `idx` 张（对 `lcd_gallery_count()` 取模）。 */
status_t lcd_gallery_show_index(st7789_t *lcd, uint8_t idx);

/** 切到下一张（循环）。 */
void lcd_gallery_next(void);

/** 当前索引（0 .. count-1；count 为 0 时为 0）。 */
uint8_t lcd_gallery_current(void);

/** 校验路径下 BMP 头与字段（BI_RGB 24/32），不刷屏。 */
status_t lcd_gallery_probe_bmp(const char *path);

/** 全屏显示指定 BMP（与图库 BMP 子集规则一致）；须 LCD 已初始化且 SD 已挂载。 */
status_t lcd_gallery_show_bmp_path(st7789_t *lcd, const char *path);

/** 全屏显示 SD 上 `.bmp`（BI_RGB）或 `.bin`（RGB565）；与图库规则一致。 */
status_t lcd_gallery_show_path(st7789_t *lcd, const char *path);

/** 在已扫描列表中按根文件名（不含路径）查找，未找到返回 `LCD_GALLERY_INDEX_NONE`。 */
#define LCD_GALLERY_INDEX_NONE (0xFFU)
uint8_t lcd_gallery_find_index_by_basename(const char *basename);

/** 设置当前图库索引（仅按键切图语义；`count==0` 时不修改）。 */
void lcd_gallery_set_current_index(uint8_t idx);

/** 投递全屏显示请求（`app_mod` 执行；播放视频时会先停止视频）。 */
bool lcd_gallery_post_show_path(const char *path);

/** 按 `picture/` 下文件名投递显示（可仅 basename）。 */
bool lcd_gallery_post_show_name(const char *name);

bool lcd_gallery_peek_pending_show(void);

/** 取走一条待显示路径（非阻塞）。 */
bool lcd_gallery_take_pending_show(char *path_out, size_t path_cap);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_LCD_GALLERY_H */
