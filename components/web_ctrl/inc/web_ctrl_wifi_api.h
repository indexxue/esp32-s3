/**
 * @file web_ctrl_wifi_api.h
 * @brief SoftAP 网页配网 REST（/api/wifi/... 与 GET /provision）。
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief 向已启动的 `httpd` 注册 Wi‑Fi 配网 URI。
 * @note 可对**新** server 重复调用（httpd stop/start 后须再注册）；扫描任务等基础设施只建一次。
 */
esp_err_t web_ctrl_wifi_api_register(httpd_handle_t server);

/** 保存/断开 STA 写 NVS 前调用（应用可注册：停 MJPEG、释放摄像头 Web 资源等）。 */
typedef void (*web_ctrl_wifi_prepare_fn)(void);
void web_ctrl_wifi_set_prepare_hook(web_ctrl_wifi_prepare_fn fn);

/**
 * @brief Wi‑Fi 扫描开始前调用（仅暂停长连接流，不触发 reboot 准备）。
 * @note 未设置时回退调用 prepare_hook（若存在）。
 */
void web_ctrl_wifi_set_pause_hook(web_ctrl_wifi_prepare_fn fn);
