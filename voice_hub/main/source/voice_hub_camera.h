/**
 * @file voice_hub_camera.h
 * @brief OV2640 低帧率预览 + 按需 JPEG 骨架（M3/M4）。
 */

#pragma once

#include "type.h"

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
