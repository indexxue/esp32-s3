/**
 * @file voice_hub_camera.h
 * @brief OV2640 低帧率预览 + 按需 JPEG 骨架（M3/M4）。
 */

#pragma once

#include "type.h"

status_t voice_hub_camera_init(void);
bool_t voice_hub_camera_is_ready(void);
status_t voice_hub_camera_preview_start(void);
status_t voice_hub_camera_preview_stop(void);
status_t voice_hub_camera_capture_jpeg_to_sd(void);
