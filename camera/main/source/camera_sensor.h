/**
 * @file camera_sensor.h
 * @brief OV2640 低帧率预览（LCD 本地显示 + Web JPEG 快照）。
 */

#pragma once

#include "type.h"

/** 预览 240×240 RGB565，LCD 240×135 居中裁剪显示。 */
#define CAMERA_SENSOR_PREVIEW_WIDTH (240U)
#define CAMERA_SENSOR_PREVIEW_HEIGHT (240U)
#define CAMERA_SENSOR_SENSOR_FORMAT "DVP_8bit_20Minput_RGB565_BE_240x240_25fps"

/** Web JPEG 轮询最小间隔（ms）；须大于单帧编码+传输耗时，避免 httpd 连接堆叠。 */
#define CAMERA_SENSOR_WEB_FRAME_MS (200U)

/** Web JPEG 质量 1–100。 */
#define CAMERA_SENSOR_WEB_JPEG_QUALITY (65U)

/**
 * LCD blit SPI 条带字节上限（须为整行 RGB565 的倍数）。
 * 全板 BOARD_ST7789_SPI_MAX_TX 常为 32KiB，与 WiFi 共用 MALLOC_CAP_DMA 时易耗尽导致重启。
 */
#ifndef CAMERA_SENSOR_BLIT_STRIP_BYTES_MAX
#define CAMERA_SENSOR_BLIT_STRIP_BYTES_MAX (8192U)
#endif

/** 预览 RGB565 快照字节数（240×240×2）。 */
#define CAMERA_SENSOR_SNAPSHOT_BYTES \
    ((uint32_t)CAMERA_SENSOR_PREVIEW_WIDTH * (uint32_t)CAMERA_SENSOR_PREVIEW_HEIGHT * 2U)

/**
 * 摄像头画面方向（仅 LCD 本地预览）。
 * 若镜像/上下颠倒，只需改此处宏后重新编译。
 */
#ifndef CAMERA_SENSOR_FLIP_VERTICAL
#define CAMERA_SENSOR_FLIP_VERTICAL 1
#endif
#ifndef CAMERA_SENSOR_FLIP_HORIZONTAL
#define CAMERA_SENSOR_FLIP_HORIZONTAL 0
#endif

/** ST7789 使用 MADCTL_BGR 时置 1，将 RGB565 转为面板 BGR565（仅 LCD 路径）。 */
#ifndef CAMERA_SENSOR_PANEL_SWAP_RB
#define CAMERA_SENSOR_PANEL_SWAP_RB 1
#endif

status_t camera_sensor_init(void);
bool_t camera_sensor_is_ready(void);
status_t camera_sensor_preview_start(void);
status_t camera_sensor_preview_stop(void);
void camera_sensor_prepare_for_reboot(void);

/** 获取最新快照（RGB565, 240×240, Big-Endian）。
 *  返回指针有效期至下一帧回调。无帧时返回 NULL。 */
const uint8_t *camera_sensor_get_snapshot(void);
uint16_t camera_sensor_get_snapshot_width(void);
uint16_t camera_sensor_get_snapshot_height(void);

/** 将最新 RGB565 帧快照编码为 JPEG（供网页预览/拍照下载）；`out_len` 为实际 JPEG 字节数。 */
status_t camera_sensor_snapshot_jpeg(uint8_t *out, uint32_t out_cap, uint32_t *out_len);
