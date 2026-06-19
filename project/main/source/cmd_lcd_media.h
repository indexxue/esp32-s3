/**
 * @file cmd_lcd_media.h
 * @brief 工程专用厂测命令：LCD 图库/视频（依赖 `source/lcd_gallery`、`source/lcd_video`）。
 */

#pragma once

/** 注册 lcdbmp/lcdshow/video* 等命令；须在 `cmd_register_defaults` 之后调用。 */
void cmd_lcd_media_register(void);
