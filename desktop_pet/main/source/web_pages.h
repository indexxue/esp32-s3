/**
 * @file web_pages.h
 * @brief desktop_pet HTTP：`GET /` 与 SD 卡文件管理 REST API。
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** `GET /`：返回 `source/www/index.html`。 */
esp_err_t web_pages_root_get_handler(httpd_req_t *req);

/** 注册 SD 列表 / 上传 / 下载 / 建目录 / 删除 / 皮肤 zip（挂到 `web_ctrl_config_t.gallery_http_register`）。 */
esp_err_t web_pages_sd_http_register(httpd_handle_t server);
