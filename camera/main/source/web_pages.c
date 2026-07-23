/**
 * @file web_pages.c
 * @brief camera HTTP 页面与 REST：根页面、状态、JPEG 快照（网页预览 + 拍照下载共用）。
 */

#include "web_pages.h"

#include "camera_sensor.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "net_wifi.h"
#include "web_ctrl_wifi_api.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static const char *TAG = "web_pages";

#define CAMERA_WEB_JPEG_BUF_CAP (49152U)

static volatile bool s_web_camera_paused;

static void web_pages_wifi_prepare_hook(void)
{
    s_web_camera_paused = true;
    camera_sensor_prepare_for_reboot();
}

static esp_err_t web_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_sensitive_forbidden(httpd_req_t *req)
{
    (void)httpd_resp_set_status(req, "403 Forbidden");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"forbidden\"}", HTTPD_RESP_USE_STRLEN);
}

/** 摄像头端点：SoftAP 子网或 STA 同子网均放行（STA 模式下也能预览/拍照）。 */
static bool web_local_peer_allowed(httpd_req_t *req)
{
    return net_wifi_http_peer_on_local_subnet(httpd_req_to_sockfd(req));
}

static esp_err_t web_pages_register_uri(httpd_handle_t server, const httpd_uri_t *uri)
{
    esp_err_t err;

    if ((server == NULL) || (uri == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s", uri->uri, esp_err_to_name(err));
    }
    return err;
}

esp_err_t web_pages_root_get_handler(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, len);
}

static esp_err_t web_favicon_get_handler(httpd_req_t *req)
{
    (void)req;
    (void)httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0U);
}

static esp_err_t web_api_status_get(httpd_req_t *req)
{
    char body[128];

    (void)snprintf(body,
                   sizeof(body),
                   "{\"product\":\"camera\",\"camera\":%s,\"width\":%u,\"height\":%u}",
                   (camera_sensor_is_ready() != FALSE) ? "true" : "false",
                   (unsigned)camera_sensor_get_snapshot_width(),
                   (unsigned)camera_sensor_get_snapshot_height());
    return web_send_json(req, body);
}

static esp_err_t web_camera_check_ready(httpd_req_t *req)
{
    if (camera_sensor_is_ready() == FALSE) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        (void)httpd_resp_send(req, "{\"ok\":false,\"error\":\"camera_not_ready\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    if (s_web_camera_paused) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        (void)httpd_resp_send(req, "{\"ok\":false,\"error\":\"camera_paused_wifi\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** 网页预览与「拍照保存」共用：返回最新帧 JPEG。 */
static esp_err_t web_api_camera_jpeg_get(httpd_req_t *req)
{
    uint8_t *jpeg_buf;
    uint32_t jpeg_len = 0U;

    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
    if (web_camera_check_ready(req) != ESP_OK) {
        return ESP_FAIL;
    }

    jpeg_buf = (uint8_t *)heap_caps_malloc(CAMERA_WEB_JPEG_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg_buf == NULL) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_memory\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (camera_sensor_snapshot_jpeg(jpeg_buf, CAMERA_WEB_JPEG_BUF_CAP, &jpeg_len) != STATUS_OK) {
        heap_caps_free(jpeg_buf);
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_frame\"}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_type(req, "image/jpeg");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_send(req, (const char *)jpeg_buf, jpeg_len);
    heap_caps_free(jpeg_buf);
    return ESP_OK;
}

static esp_err_t web_api_camera_resume_post(httpd_req_t *req)
{
    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
    s_web_camera_paused = false;
    return web_send_json(req, "{\"ok\":true}");
}

esp_err_t web_pages_register(httpd_handle_t server)
{
    httpd_uri_t status = {
        .uri     = "/api/camera/status",
        .method  = HTTP_GET,
        .handler = web_api_status_get,
    };
    httpd_uri_t favicon = {
        .uri     = "/favicon.ico",
        .method  = HTTP_GET,
        .handler = web_favicon_get_handler,
    };
    httpd_uri_t camera_jpeg = {
        .uri     = "/api/camera/camera.jpg",
        .method  = HTTP_GET,
        .handler = web_api_camera_jpeg_get,
    };
    httpd_uri_t camera_resume = {
        .uri     = "/api/camera/camera_resume",
        .method  = HTTP_POST,
        .handler = web_api_camera_resume_post,
    };
    esp_err_t err;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    web_ctrl_wifi_set_prepare_hook(web_pages_wifi_prepare_hook);

    err = web_pages_register_uri(server, &favicon);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &status);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &camera_jpeg);
    if (err != ESP_OK) {
        return err;
    }
    return web_pages_register_uri(server, &camera_resume);
}
