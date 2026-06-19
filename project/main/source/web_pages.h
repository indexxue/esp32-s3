/**
 * @file web_pages.h
 * @brief 本工程 HTTP 页面（`source/www/index.html` 嵌入固件）。
 */

#pragma once

#include "esp_http_server.h"

/** `GET /`：返回 `source/www/index.html`。 */
esp_err_t web_pages_root_get_handler(httpd_req_t *req);
