/**
 * @file web_ctrl_wifi_api.h
 * @brief SoftAP 网页配网 REST（/api/wifi/... 与 GET /provision）。
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief 向已启动的 `httpd` 注册 Wi‑Fi 配网 URI；可重复调用（仅首次生效）。
 */
esp_err_t web_ctrl_wifi_api_register(httpd_handle_t server);
