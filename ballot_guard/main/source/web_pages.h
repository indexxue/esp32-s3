/**
 * @file web_pages.h
 * @brief ballot_guard HTTP 页面（`source/www/index.html` 嵌入固件）。
 */

#pragma once

#include "esp_http_server.h"

/** `GET /`：返回 `source/www/index.html`。 */
esp_err_t web_pages_root_get_handler(httpd_req_t *req);

/** 注册 ballot_guard 专用 REST（vote / lcd menu API）；须在 web_ctrl_start 成功后调用。 */
esp_err_t web_pages_register(httpd_handle_t server);
