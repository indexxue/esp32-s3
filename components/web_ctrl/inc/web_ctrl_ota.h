/**
 * @file web_ctrl_ota.h
 * @brief SoftAP 本地 OTA HTTP 路由（维护页与 api/ota REST），见 doc/ota_development_plan.md。
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** 在 `web_server_start` 之后注册 OTA URI；未启用 Kconfig `WEB_CTRL_OTA` 时为空操作。 */
esp_err_t web_ctrl_ota_register(httpd_handle_t server);
