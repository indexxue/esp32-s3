/**
 * @file web_server.c
 * @brief HTTP 服务：`GET /api/health`、`POST /api/cmd`；`GET /` 由应用通过 `root_get_handler` 提供。
 */

#include "web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#ifndef CONFIG_WEB_CTRL_CMD_SYNC_TIMEOUT_MS
#define CONFIG_WEB_CTRL_CMD_SYNC_TIMEOUT_MS (3000)
#endif

#include "esp_http_server.h"
#include "esp_log.h"

#include "web_ctrl_cmd.h"
#include "web_bmp_upload.h"
#include "web_video.h"

#include "cmd.h"

static const char *TAG = "web_server";

#define WEB_CMD_BODY_MAX (256U)
#define WEB_CMD_JSON_OUT (1024U)

static httpd_handle_t s_server;

static esp_err_t health_get_handler(httpd_req_t *req)
{
    static const char json[] = "{\"ok\":true}";

    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static bool json_skip_ws(const char **pp)
{
    const char *p = *pp;

    if (p == NULL) {
        return false;
    }
    while ((*p != '\0') && (isspace((unsigned char)*p) != 0)) {
        p++;
    }
    *pp = p;
    return (*p != '\0');
}

static bool json_extract_line_field(const char *body, char *out_line, size_t out_cap)
{
    static const char keypat[] = "\"line\"";
    const char       *found = strstr(body, keypat);
    const char       *colon;
    const char       *p;
    size_t            o = 0U;

    if ((found == NULL) || (out_cap == 0U)) {
        return false;
    }
    colon = strchr(found + sizeof(keypat) - 1U, ':');
    if (colon == NULL) {
        return false;
    }
    p = colon + 1U;
    if (!json_skip_ws(&p)) {
        return false;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    while ((*p != '\0') && (*p != '"')) {
        if ((*p == '\\') && (p[1] != '\0')) {
            p++;
        }
        if (o + 1U >= out_cap) {
            return false;
        }
        out_line[o++] = *p++;
    }
    if (*p != '"') {
        return false;
    }
    out_line[o] = '\0';
    return (*p == '"');
}

static size_t json_escape_to_buf(const char *src, char *dst, size_t dst_cap)
{
    size_t j = 0U;

    if ((src == NULL) || (dst == NULL) || (dst_cap == 0U)) {
        return 0U;
    }
    for (size_t i = 0U; (src[i] != '\0') && (j + 1U < dst_cap); i++) {
        const unsigned char c = (unsigned char)src[i];

        if ((c == '"') || (c == '\\')) {
            if (j + 2U >= dst_cap) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c < 0x20U) {
            int w = snprintf((char *)(dst + j), dst_cap - j, "\\u%04x", (unsigned int)c);
            if ((w <= 0) || ((size_t)w >= dst_cap - j)) {
                break;
            }
            j += (size_t)w;
        } else {
            dst[j++] = (char)c;
        }
    }
    if (j < dst_cap) {
        dst[j] = '\0';
    } else {
        dst[dst_cap - 1U] = '\0';
    }
    return j;
}

/** 读 POST body：依 `req->content_len` 循环 recv（与 `web_ctrl_wifi_api` 一致）。 */
static esp_err_t web_http_read_post_body(httpd_req_t *req, char *body, size_t body_cap, size_t *out_len)
{
    size_t body_len;
    size_t total;
    int    rlen;

    if ((out_len == NULL) || (body_cap <= 1U)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    body_len = (size_t)req->content_len;
    if (body_len == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (body_len >= body_cap) {
        return ESP_ERR_NO_MEM;
    }
    total = 0U;
    while (total < body_len) {
        rlen = httpd_req_recv(req, body + total, body_len - total);
        if (rlen < 0) {
            ESP_LOGW(TAG, "cmd post recv err %d", rlen);
            return ESP_FAIL;
        }
        if (rlen == 0) {
            return ESP_FAIL;
        }
        total += (size_t)rlen;
    }
    body[body_len] = '\0';
    *out_len = body_len;
    return ESP_OK;
}

static esp_err_t cmd_post_handler(httpd_req_t *req)
{
    char              body[WEB_CMD_BODY_MAX + 1U];
    char              line[CMD_LINE_MAX];
    char              reply[WEB_CTRL_CMD_REPLY_MAX];
    char              json[WEB_CMD_JSON_OUT];
    size_t            body_len = 0U;
    size_t            out_len  = 0U;
    esp_err_t         rbody;
    esp_err_t         ex;
    const int         tmo_ms = CONFIG_WEB_CTRL_CMD_SYNC_TIMEOUT_MS;

    rbody = web_http_read_post_body(req, body, sizeof(body), &body_len);
    if (rbody == ESP_ERR_INVALID_SIZE) {
        (void)httpd_resp_set_status(req, "411 Length Required");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need body (content_len 0)\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (rbody == ESP_ERR_NO_MEM) {
        (void)httpd_resp_set_status(req, "413 Payload Too Large");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"body too large\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (rbody != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"recv\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!json_extract_line_field(body, line, sizeof(line))) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need JSON {\\\"line\\\":\\\"...\\\"}\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    ex = web_ctrl_cmd_execute_sync(line, reply, sizeof(reply), &out_len, tmo_ms);
    if (ex == ESP_ERR_INVALID_STATE) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"busy\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (ex == ESP_ERR_TIMEOUT) {
        (void)httpd_resp_set_status(req, "504 Gateway Timeout");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"timeout\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (ex != ESP_OK) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"exec\"}", HTTPD_RESP_USE_STRLEN);
    }

    {
        static const char prefix[] = "{\"ok\":true,\"reply\":\"";
        const size_t      plen      = sizeof(prefix) - 1U;
        const size_t      tail_room = (sizeof(json) > plen + 3U) ? (sizeof(json) - plen - 3U) : 0U;
        size_t            j;

        if (tail_room < 1U) {
            (void)httpd_resp_set_status(req, "500 Internal Server Error");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"buffer\"}", HTTPD_RESP_USE_STRLEN);
        }
        (void)memcpy(json, prefix, plen);
        j = json_escape_to_buf(reply, json + plen, tail_room + 1U);
        if (j > tail_room) {
            j = tail_room;
        }
        json[plen + j]       = '"';
        json[plen + j + 1U] = '}';
        json[plen + j + 2U] = '\0';
    }

    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(uint16_t port, web_root_handler_fn root_get_handler)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = (port == 0U) ? 80U : port;
    /* 默认 8 槽：`web_server` 3 个 + `web_ctrl_wifi_api` 7 个会溢出，须加大。 */
    config.max_uri_handlers = 32U;
    /* 默认栈 4096：`wifi_scan_result_get_handler` 等单帧 JSON 约 4KB，会栈溢出破坏 httpd 会话表。 */
    config.stack_size = 12288U;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uri_health = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_cmd = {
        .uri = "/api/cmd",
        .method = HTTP_POST,
        .handler = cmd_post_handler,
        .user_ctx = NULL,
    };

    if (root_get_handler != NULL) {
        const httpd_uri_t uri_root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL,
        };

        err = httpd_register_uri_handler(s_server, &uri_root);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register / failed: %s", esp_err_to_name(err));
            (void)httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    } else {
        ESP_LOGW(TAG, "no GET / handler; register web_pages_root_get_handler in app config");
    }

    err = httpd_register_uri_handler(s_server, &uri_health);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/health failed: %s", esp_err_to_name(err));
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    err = httpd_register_uri_handler(s_server, &uri_cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/cmd failed: %s", esp_err_to_name(err));
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    err = web_bmp_upload_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/upload/bmp failed: %s", esp_err_to_name(err));
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    err = web_video_register(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register video API failed: %s", esp_err_to_name(err));
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "HTTP listening on port %u", (unsigned int)config.server_port);
    return ESP_OK;
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}

esp_err_t web_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_stop: %s", esp_err_to_name(err));
    }
    s_server = NULL;
    return ESP_OK;
}

bool web_server_is_running(void)
{
    return s_server != NULL;
}
