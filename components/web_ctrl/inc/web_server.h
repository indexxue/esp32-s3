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

/**
 * @brief 启动 HTTP 服务；已运行则返回 `ESP_ERR_INVALID_STATE`。
 */
esp_err_t web_server_start(uint16_t port);

esp_err_t web_server_stop(void);

bool web_server_is_running(void);
