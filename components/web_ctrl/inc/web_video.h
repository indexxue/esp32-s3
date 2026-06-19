/**
 * @file web_video.h
 * @brief SD 视频 HTTP：上传、列表、封面预览、LCD 播放/停止。
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t web_video_register(httpd_handle_t server);
