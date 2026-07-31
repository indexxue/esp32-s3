/**
 * @file camera_sensor.h
 * @brief OV2640 预览（LCD + Web JPEG/MJPEG；拉流时独占关 LCD）。
 */

#pragma once

#include "type.h"

#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 传感器采集：240×240 RGB565 @ ~25fps（与 voice_hub 同格式）。
 * 相对 640×480@6fps 更跟手；CIF 开窗偶发四周偏紫，以实机为准。
 */
#define CAMERA_SENSOR_CAPTURE_WIDTH (240U)
#define CAMERA_SENSOR_CAPTURE_HEIGHT (240U)
#define CAMERA_SENSOR_SENSOR_FORMAT "DVP_8bit_20Minput_RGB565_BE_240x240_25fps"

#define CAMERA_SENSOR_WEB_DEFAULT_WIDTH (240U)
#define CAMERA_SENSOR_WEB_DEFAULT_HEIGHT (240U)

#define CAMERA_SENSOR_PREVIEW_WIDTH CAMERA_SENSOR_WEB_DEFAULT_WIDTH
#define CAMERA_SENSOR_PREVIEW_HEIGHT CAMERA_SENSOR_WEB_DEFAULT_HEIGHT

/** 与 ~25fps 采集对齐的网页帧间隔提示。 */
#define CAMERA_SENSOR_WEB_FRAME_MS (40U)
#define CAMERA_SENSOR_WEB_JPEG_QUALITY (55U)

#ifndef CAMERA_SENSOR_LCD_MIN_INTERVAL_MS
#define CAMERA_SENSOR_LCD_MIN_INTERVAL_MS (160U)
#endif

/** 网页 MJPEG 流在线时降低 LCD 刷新，把 CPU/SPI 让给编码与 Wi‑Fi。 */
#ifndef CAMERA_SENSOR_LCD_STREAM_INTERVAL_MS
#define CAMERA_SENSOR_LCD_STREAM_INTERVAL_MS (500U)
#endif

#ifndef CAMERA_SENSOR_SNAPSHOT_MIN_INTERVAL_MS
#define CAMERA_SENSOR_SNAPSHOT_MIN_INTERVAL_MS (40U)
#endif

/** MJPEG 在线时 snapshot/JPEG 目标约 10fps，把 PSRAM/CPU 让给检测减叠框滞后。 */
#ifndef CAMERA_SENSOR_SNAPSHOT_STREAM_INTERVAL_MS
#define CAMERA_SENSOR_SNAPSHOT_STREAM_INTERVAL_MS (100U)
#endif

/** 无 MJPEG 时 JPEG cache 刷新间隔（snapshot 仍按 SNAPSHOT_MIN 供检测）。 */
#ifndef CAMERA_SENSOR_JPEG_IDLE_INTERVAL_MS
#define CAMERA_SENSOR_JPEG_IDLE_INTERVAL_MS (200U)
#endif

#ifndef CAMERA_SENSOR_JPEG_CACHE_CAP
#define CAMERA_SENSOR_JPEG_CACHE_CAP (49152U)
#endif

#ifndef CAMERA_SENSOR_BLIT_STRIP_BYTES_MAX
#define CAMERA_SENSOR_BLIT_STRIP_BYTES_MAX (8192U)
#endif

#define CAMERA_SENSOR_SNAPSHOT_BYTES_MAX \
    ((uint32_t)CAMERA_SENSOR_CAPTURE_WIDTH * (uint32_t)CAMERA_SENSOR_CAPTURE_HEIGHT * 2U)

#ifndef CAMERA_SENSOR_FLIP_VERTICAL
#define CAMERA_SENSOR_FLIP_VERTICAL 1
#endif
#ifndef CAMERA_SENSOR_FLIP_HORIZONTAL
#define CAMERA_SENSOR_FLIP_HORIZONTAL 0
#endif

#ifndef CAMERA_SENSOR_PANEL_SWAP_RB
#define CAMERA_SENSOR_PANEL_SWAP_RB 1
#endif

status_t camera_sensor_init(void);
bool_t camera_sensor_is_ready(void);
status_t camera_sensor_preview_start(void);
status_t camera_sensor_preview_stop(void);
void camera_sensor_prepare_for_reboot(void);

uint32_t camera_sensor_get_frame_count(void);
uint32_t camera_sensor_get_lcd_blit_count(void);

uint16_t camera_sensor_get_snapshot_width(void);
uint16_t camera_sensor_get_snapshot_height(void);
uint16_t camera_sensor_get_web_width(void);
uint16_t camera_sensor_get_web_height(void);
uint8_t camera_sensor_get_jpeg_quality(void);
uint32_t camera_sensor_get_web_poll_ms(void);
uint8_t camera_sensor_get_grayscale(void);
uint8_t camera_sensor_get_zoom(void);
uint8_t camera_sensor_get_img_rotate(void); /* 0/1/2/3 → 0°/90°/180°/270° */
uint8_t camera_sensor_get_flip_v(void);
uint8_t camera_sensor_get_flip_h(void);
uint32_t camera_sensor_get_jpeg_seq(void);

/**
 * MJPEG 客户端进入/离开：进入独占预览（关 LCD blit/背光，跳过 WEB 叠框），
 * 末个客户端离开后恢复 LCD。
 */
void camera_sensor_web_stream_enter(void);
void camera_sensor_web_stream_leave(void);

/** 是否有网页 MJPEG 独占预览在线。 */
bool_t camera_sensor_web_stream_active(void);

/**
 * 等待 JPEG cache 序号变化（新帧就绪）。
 * @param inout_seq 入：上次序号；出：新序号。首次可传 0。
 */
status_t camera_sensor_wait_jpeg_seq(uint32_t *inout_seq, uint32_t timeout_ms);

/**
 * 应用并可选写入 NVS 的摄像头显示配置。
 * @param cfg     完整配置（可用 nvs_camera_settings_default 填默认）
 * @param persist true 时写入 NVS
 */
status_t camera_sensor_apply_settings(const nvs_camera_settings_t *cfg, bool_t persist);

/** 兼容：只改网页分辨率/画质并持久化。 */
status_t camera_sensor_set_web_cfg(uint16_t width, uint16_t height, uint8_t quality);

status_t camera_sensor_copy_jpeg_cache(uint8_t *out, uint32_t out_cap, uint32_t *out_len);
status_t camera_sensor_snapshot_jpeg(uint8_t *out, uint32_t out_cap, uint32_t *out_len);

/**
 * 拷贝当前 RGB565 snapshot（与网页/LCD 同源）。
 * @param out      输出缓冲
 * @param out_cap  容量（字节）
 * @param out_w/h  输出宽高
 */
status_t camera_sensor_copy_rgb565(uint8_t *out, uint32_t out_cap, uint16_t *out_w, uint16_t *out_h);

/**
 * 零拷贝借用已发布 snapshot 槽（双槽：检测持有期间预览写另一槽）。
 * 用完必须 camera_sensor_release_rgb565()。
 */
status_t camera_sensor_acquire_rgb565(const uint8_t **out, uint16_t *out_w, uint16_t *out_h, uint32_t *gen);
void camera_sensor_release_rgb565(void);

#ifdef __cplusplus
}
#endif
