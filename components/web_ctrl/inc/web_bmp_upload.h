/**
 * @file web_bmp_upload.h
 * @brief 注册 BMP 上传与 SD 图库 HTTP：上传、列表、预览、删除、偏好（开机默认图）、`POST /api/gallery/show`（LCD 显示）。
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t web_bmp_upload_register(httpd_handle_t server);
