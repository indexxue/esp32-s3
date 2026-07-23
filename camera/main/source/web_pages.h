/**
 * @file web_pages.h
 * @brief camera HTTP 页面与 REST：网页预览 + 拍照下载。
 */

#pragma once

#include "esp_http_server.h"

esp_err_t web_pages_root_get_handler(httpd_req_t *req);
esp_err_t web_pages_register(httpd_handle_t server);
