/**
 * @file web_ctrl.h
 * @brief Web 控制栈对外入口：Wi‑Fi + HTTP 编排（`components/web_ctrl`）。
 *
 * 使用顺序建议：
 * 1. `nvs_flash_init()` 已由应用完成；
 * 2. `web_ctrl_config_init_defaults()` 填默认配置并按需改 SSID/密码/端口；
 * 3. `web_ctrl_start()`；关机或调试结束时 `web_ctrl_stop()`。
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "net_wifi.h"
#include "web_server.h"

/** 可选 HTTP 路由注册（由工程 `source/` 提供，如 gallery/video API）。 */
typedef esp_err_t (*web_media_http_register_fn)(httpd_handle_t server);

typedef struct {
    net_wifi_config_t wifi;
    /** HTTP 监听端口；0 表示默认 80。 */
    uint16_t http_port;
    /** 应用层 `GET /` 页面处理器；`NULL` 则不注册根路径（仅 REST API）。 */
    web_root_handler_fn root_get_handler;
    /** 可选 gallery REST 路由；`NULL` 则不注册。 */
    web_media_http_register_fn gallery_http_register;
    /** 可选 video REST 路由；`NULL` 则不注册。 */
    web_media_http_register_fn video_http_register;
} web_ctrl_config_t;

void web_ctrl_config_init_defaults(web_ctrl_config_t *cfg);

/** 若 NVS 中存在有效 `nvs_web_ctrl_settings_t`，则覆盖 `cfg` 中 SoftAP、STA 与 HTTP 端口字段（须先 `web_ctrl_config_init_defaults`）。 */
void web_ctrl_config_merge_nvs(web_ctrl_config_t *cfg);

esp_err_t web_ctrl_start(const web_ctrl_config_t *cfg);

esp_err_t web_ctrl_stop(void);

bool web_ctrl_is_running(void);
