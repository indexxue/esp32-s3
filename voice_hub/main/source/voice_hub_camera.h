/**
 * @file voice_hub_camera.h
 * @brief OV2640 低帧率预览 + 按需 JPEG 骨架（M3/M4）。
 */

#pragma once

#include "type.h"

/** 预览 240×240 RGB565，LCD 240×135 居中裁剪显示。 */
#define VOICE_HUB_CAMERA_PREVIEW_WIDTH (240U)
#define VOICE_HUB_CAMERA_PREVIEW_HEIGHT (240U)
#define VOICE_HUB_CAMERA_SENSOR_FORMAT "DVP_8bit_20Minput_RGB565_BE_240x240_25fps"

/** Web JPEG 轮询最小间隔（ms）；须大于单帧编码+传输耗时，避免 httpd 连接堆叠。 */
#define VOICE_HUB_CAMERA_WEB_FRAME_MS (200U)

/** Web JPEG 质量 1–100。 */
#define VOICE_HUB_CAMERA_WEB_JPEG_QUALITY (65U)

/**
 * LCD blit SPI 条带字节上限（须为整行 RGB565 的倍数）。
 * 全板 BOARD_ST7789_SPI_MAX_TX 常为 32KiB，与 WiFi 共用 MALLOC_CAP_DMA 时易耗尽导致重启。
 */
#ifndef VOICE_HUB_CAMERA_BLIT_STRIP_BYTES_MAX
#define VOICE_HUB_CAMERA_BLIT_STRIP_BYTES_MAX (8192U)
#endif

/** 预览/Web 共用 RGB565 快照字节数（240×240×2）。 */
#define VOICE_HUB_CAMERA_SNAPSHOT_BYTES \
    ((uint32_t)VOICE_HUB_CAMERA_PREVIEW_WIDTH * (uint32_t)VOICE_HUB_CAMERA_PREVIEW_HEIGHT * 2U)

/**
 * 摄像头画面方向（LCD 与 Web 共用同一套归一化处理，保证一致）。
 * 若镜像/上下颠倒，只需改此处宏后重新编译。
 */
#ifndef VOICE_HUB_CAMERA_FLIP_VERTICAL
#define VOICE_HUB_CAMERA_FLIP_VERTICAL 1
#endif
#ifndef VOICE_HUB_CAMERA_FLIP_HORIZONTAL
#define VOICE_HUB_CAMERA_FLIP_HORIZONTAL 0
#endif

/** ST7789 使用 MADCTL_BGR 时置 1，将 RGB565 转为面板 BGR565（仅 LCD 路径）。 */
#ifndef VOICE_HUB_CAMERA_PANEL_SWAP_RB
#define VOICE_HUB_CAMERA_PANEL_SWAP_RB 1
#endif

typedef struct {
    bool_t flip_vertical;
    bool_t flip_horizontal;
    u16_t rotate_deg;
} voice_hub_camera_view_t;

status_t voice_hub_camera_init(void);
bool_t voice_hub_camera_is_ready(void);
status_t voice_hub_camera_preview_start(void);
status_t voice_hub_camera_preview_stop(void);
void voice_hub_camera_prepare_for_reboot(void);
status_t voice_hub_camera_capture_jpeg_to_sd(void);

void voice_hub_camera_view_get(voice_hub_camera_view_t *out);
status_t voice_hub_camera_view_set(const voice_hub_camera_view_t *view);

/** 顺时针旋转预览 90°（0→90→180→270→0），LCD 与网页同步。 */
status_t voice_hub_camera_view_rotate_cw(void);

/** 将最新 RGB565 帧快照编码为 JPEG（供 Web 预览）；`out_len` 为实际 JPEG 字节数。 */
status_t voice_hub_camera_snapshot_jpeg(uint8_t *out, uint32_t out_cap, uint32_t *out_len);
