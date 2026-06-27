#include "web_pages.h"

#include "voice_hub_config.h"
#include "voice_hub_audio.h"
#include "voice_hub_camera.h"

#include <stdio.h>
#include <string.h>

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t web_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_pages_root_get_handler(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, len);
}

static esp_err_t web_api_status_get(httpd_req_t *req)
{
    char body[256];

    (void)snprintf(body,
                   sizeof(body),
                   "{\"product\":\"voice_hub\",\"audio\":%s,\"camera\":%s,\"intercom\":%s}",
                   voice_hub_audio_is_ready() ? "true" : "false",
                   voice_hub_camera_is_ready() ? "true" : "false",
#if VOICE_HUB_ENABLE_INTERCOM
                   "false");
#else
                   "false");
#endif
    return web_send_json(req, body);
}

static esp_err_t web_api_intercom_post(httpd_req_t *req)
{
    char body[64];

    if (httpd_req_recv(req, body, sizeof(body) - 1U) <= 0) {
        return ESP_FAIL;
    }
    body[sizeof(body) - 1U] = '\0';

#if VOICE_HUB_ENABLE_INTERCOM
    if (strstr(body, "\"start\":true") != NULL) {
        (void)voice_hub_audio_intercom_start();
    } else {
        (void)voice_hub_audio_intercom_stop();
    }
#endif
    return web_send_json(req, "{\"ok\":true}");
}

static esp_err_t web_api_capture_post(httpd_req_t *req)
{
#if VOICE_HUB_ENABLE_CAMERA
    status_t st = voice_hub_camera_capture_jpeg_to_sd();
    if (st == STATUS_OK) {
        return web_send_json(req, "{\"ok\":true}");
    }
#endif
    return web_send_json(req, "{\"ok\":false,\"error\":\"not_ready\"}");
}

esp_err_t web_pages_register(httpd_handle_t server)
{
    httpd_uri_t status = {
        .uri     = "/api/voice_hub/status",
        .method  = HTTP_GET,
        .handler = web_api_status_get,
    };
    httpd_uri_t intercom = {
        .uri     = "/api/voice_hub/intercom",
        .method  = HTTP_POST,
        .handler = web_api_intercom_post,
    };
    httpd_uri_t capture = {
        .uri     = "/api/voice_hub/capture",
        .method  = HTTP_POST,
        .handler = web_api_capture_post,
    };

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &intercom));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &capture));
    return ESP_OK;
}
