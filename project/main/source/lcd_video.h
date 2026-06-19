/**
 * @file lcd_video.h
 * @brief SD 卡 MJPEG-in-AVI 视频播放（240×135 RGB565 条带推屏）。
 */

#pragma once

#include "type.h"

#include "st7789.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 与图库互斥的 UI 模式（由 app_mod 维护）。 */
typedef enum {
    LCD_UI_MODE_GALLERY = 0,
    LCD_UI_MODE_VIDEO   = 1,
} lcd_ui_mode_t;

/** SD 卡视频目录（`BOARD_SDCARD_MOUNT_POINT/videos`）。 */
#define LCD_VIDEO_DIR_SUFFIX "/videos"

#define LCD_VIDEO_INDEX_NONE (0xFFU)

/** Scan BOARD_SDCARD_MOUNT_POINT/videos for .avi files; sort by name. */
status_t lcd_video_scan(void);

uint8_t lcd_video_count(void);

/** 第 `idx` 条路径写入 `out`（对 count 取模）；`out_cap` 建议 ≥320。 */
status_t lcd_video_path_at(uint8_t idx, char *out, size_t out_cap);

/** 阻塞播放直至结束、停止或错误；内部循环播放 AVI。 */
status_t lcd_video_play_path(st7789_t *lcd, const char *path);

status_t lcd_video_play_index(st7789_t *lcd, uint8_t idx);

/** 异步停止请求（播放循环内轮询）。 */
void lcd_video_request_stop(void);

bool lcd_video_is_playing(void);

/** 投递播放请求（由 cmd / 其它任务调用；app_mod 轮询 `lcd_video_take_pending_play`）。 */
bool lcd_video_post_play_path(const char *path);

bool lcd_video_post_play_index(uint8_t idx);

bool lcd_video_post_benchmark(const char *path, uint32_t max_frames);

typedef enum {
    LCD_VIDEO_PENDING_NONE = 0,
    LCD_VIDEO_PENDING_PLAY_PATH,
    LCD_VIDEO_PENDING_PLAY_INDEX,
    LCD_VIDEO_PENDING_BENCH,
} lcd_video_pending_kind_t;

/** 取走一条待播放命令（非阻塞）；返回 false 表示无待处理项。 */
bool lcd_video_take_pending_play(lcd_video_pending_kind_t *kind, char *path_out, size_t path_cap,
                                 uint8_t *index_out, uint32_t *bench_frames_out);

/**
 * M0b：连续解码+推屏基准（默认 100 帧或文件较短时播完）。
 * 串口打印 avg_fps 与 read/decode/blit 分段 ms。
 */
status_t lcd_video_benchmark(st7789_t *lcd, const char *path, uint32_t max_frames);

/** 读取 AVI 第一帧 MJPEG 载荷（用于 Web 封面预览）；`buf_cap` 建议 ≥65536。 */
status_t lcd_video_read_first_jpeg(const char *path, uint8_t *buf, size_t buf_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
