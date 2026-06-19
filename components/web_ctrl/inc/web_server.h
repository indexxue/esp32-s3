/**
 * @file web_server.h
 * @brief HTTP 服务：`GET /`、`GET /api/health`、`POST /api/cmd`（阶段 2～3）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

/** 当前 `httpd` 句柄；未启动时为 `NULL`。 */
httpd_handle_t web_server_get_handle(void);

/** 应用层 `GET /` 处理器（与 `httpd_uri_t.handler` 契约相同）。 */
typedef esp_err_t (*web_root_handler_fn)(httpd_req_t *req);

/**
 * @brief 启动 HTTP 服务；已运行则返回 `ESP_ERR_INVALID_STATE`。
 * @param root_get_handler 可选 `GET /` 处理器（由应用嵌入 `www/index.html` 等提供）；`NULL` 则不注册根路径。
 */
esp_err_t web_server_start(uint16_t port, web_root_handler_fn root_get_handler);

esp_err_t web_server_stop(void);

bool web_server_is_running(void);
