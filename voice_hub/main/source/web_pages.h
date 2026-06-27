/**
 * @file web_pages.h
 * @brief voice_hub HTTP 页面与 REST 骨架。
 */

#pragma once

#include "esp_http_server.h"

esp_err_t web_pages_root_get_handler(httpd_req_t *req);
esp_err_t web_pages_register(httpd_handle_t server);
