/**
 * @file camera_sensor.h
 * @brief OV2640 低帧率预览（LCD 本地显示 + Web JPEG 快照）。
 */

#pragma once

#include "type.h"

#include "nvs.h"

/**
 * 传感器采集分辨率。640×480 SVGA，避免 240 直出 CIF 开窗的四周偏紫。
 * 网页/LCD：zoom=1 整幅缩小；zoom>1 中心取更小窗口再缩放。
 */
#define CAMERA_SENSOR_CAPTURE_WIDTH (640U)
#define CAMERA_SENSOR_CAPTURE_HEIGHT (480U)
#define CAMERA_SENSOR_SENSOR_FORMAT "DVP_8bit_20Minput_RGB565_BE_640x480_6fps"

#define CAMERA_SENSOR_WEB_DEFAULT_WIDTH (320U)
#define CAMERA_SENSOR_WEB_DEFAULT_HEIGHT (240U)

#define CAMERA_SENSOR_PREVIEW_WIDTH CAMERA_SENSOR_WEB_DEFAULT_WIDTH
#define CAMERA_SENSOR_PREVIEW_HEIGHT CAMERA_SENSOR_WEB_DEFAULT_HEIGHT

#define CAMERA_SENSOR_WEB_FRAME_MS (120U)
#define CAMERA_SENSOR_WEB_JPEG_QUALITY (55U)

#ifndef CAMERA_SENSOR_LCD_MIN_INTERVAL_MS
#define CAMERA_SENSOR_LCD_MIN_INTERVAL_MS (160U)
#endif

/** 网页 MJPEG 流在线时降低 LCD 刷新，把 CPU/SPI 让给编码与 Wi‑Fi。 */
#ifndef CAMERA_SENSOR_LCD_STREAM_INTERVAL_MS
#define CAMERA_SENSOR_LCD_STREAM_INTERVAL_MS (500U)
#endif

#ifndef CAMERA_SENSOR_SNAPSHOT_MIN_INTERVAL_MS
#define CAMERA_SENSOR_SNAPSHOT_MIN_INTERVAL_MS (50U)
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

/** MJPEG 客户端进入/离开：用于降低 LCD 刷新优先级。 */
void camera_sensor_web_stream_enter(void);
void camera_sensor_web_stream_leave(void);

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
