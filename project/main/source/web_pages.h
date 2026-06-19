/**
 * @file web_pages.h
 * @brief 本工程 HTTP：`GET /` 页面、SD 图库与 MJPEG 视频 REST API。
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** `GET /`：返回 `source/www/index.html`。 */
esp_err_t web_pages_root_get_handler(httpd_req_t *req);

/** 注册 BMP 上传与 SD 图库 HTTP 路由。 */
esp_err_t web_bmp_upload_register(httpd_handle_t server);

/** 注册 SD 视频 HTTP 路由。 */
esp_err_t web_video_register(httpd_handle_t server);
