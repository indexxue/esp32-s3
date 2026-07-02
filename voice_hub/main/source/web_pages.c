#include "web_pages.h"

#include "voice_hub_config.h"
#include "voice_hub_camera.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "web_ctrl_wifi_api.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define VOICE_HUB_WEB_JPEG_BUF_CAP (49152U)
#define VOICE_HUB_WEB_MJPEG_BOUNDARY "frame"

static volatile bool s_mjpg_abort;
static volatile bool s_web_camera_paused;

static void web_pages_wifi_prepare_hook(void)
{
    s_mjpg_abort        = true;
    s_web_camera_paused = true;
    voice_hub_camera_prepare_for_reboot();
}

bool web_pages_web_camera_is_paused(void)
{
    return s_web_camera_paused;
}

void web_pages_web_camera_resume(void)
{
    s_web_camera_paused = false;
    s_mjpg_abort        = false;
}

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

static esp_err_t web_favicon_get_handler(httpd_req_t *req)
{
    (void)req;
    (void)httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0U);
}

static esp_err_t web_api_status_get(httpd_req_t *req)
{
    char body[256];

    (void)snprintf(body,
                   sizeof(body),
                   "{\"product\":\"voice_hub\",\"camera\":%s}",
                   voice_hub_camera_is_ready() ? "true" : "false");
    return web_send_json(req, body);
}

static esp_err_t web_api_capture_post(httpd_req_t *req)
{
#if VOICE_HUB_ENABLE_CAMERA
    status_t st = voice_hub_camera_capture_jpeg_to_sd();
    if (st == STATUS_OK) {
        return web_send_json(req, "{\"ok\":true}");
    }
#else
    (void)req;
#endif
    return web_send_json(req, "{\"ok\":false,\"error\":\"not_ready\"}");
}

#if VOICE_HUB_ENABLE_CAMERA && VOICE_HUB_ENABLE_WIFI_WEB

static esp_err_t web_camera_send_jpeg(httpd_req_t *req, const uint8_t *jpeg, uint32_t len)
{
    (void)httpd_resp_set_type(req, "image/jpeg");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)jpeg, len);
}

static esp_err_t web_camera_check_ready(httpd_req_t *req)
{
    if (!voice_hub_camera_is_ready()) {
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

static esp_err_t web_api_camera_jpeg_get(httpd_req_t *req)
{
    uint8_t *jpeg_buf;
    uint32_t jpeg_len = 0U;

    if (web_camera_check_ready(req) != ESP_OK) {
        return ESP_FAIL;
    }

    jpeg_buf = (uint8_t *)heap_caps_malloc(VOICE_HUB_WEB_JPEG_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg_buf == NULL) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_memory\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (voice_hub_camera_snapshot_jpeg(jpeg_buf, VOICE_HUB_WEB_JPEG_BUF_CAP, &jpeg_len) != STATUS_OK) {
        heap_caps_free(jpeg_buf);
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_frame\"}", HTTPD_RESP_USE_STRLEN);
    }

    (void)web_camera_send_jpeg(req, jpeg_buf, jpeg_len);
    heap_caps_free(jpeg_buf);
    return ESP_OK;
}

static esp_err_t web_api_camera_mjpg_get(httpd_req_t *req)
{
    uint8_t *jpeg_buf;
    char     part_hdr[128];
    esp_err_t err;

    if (web_camera_check_ready(req) != ESP_OK) {
        return ESP_FAIL;
    }

    jpeg_buf = (uint8_t *)heap_caps_malloc(VOICE_HUB_WEB_JPEG_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg_buf == NULL) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_memory\"}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=" VOICE_HUB_WEB_MJPEG_BOUNDARY);
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    for (;;) {
        uint32_t jpeg_len = 0U;

        if (s_mjpg_abort) {
            break;
        }

        if (voice_hub_camera_snapshot_jpeg(jpeg_buf, VOICE_HUB_WEB_JPEG_BUF_CAP, &jpeg_len) == STATUS_OK) {
            (void)snprintf(part_hdr,
                           sizeof(part_hdr),
                           "\r\n--" VOICE_HUB_WEB_MJPEG_BOUNDARY "\r\n"
                           "Content-Type: image/jpeg\r\n"
                           "Content-Length: %u\r\n\r\n",
                           (unsigned)jpeg_len);
            err = httpd_resp_send_chunk(req, part_hdr, HTTPD_RESP_USE_STRLEN);
            if (err == ESP_OK) {
                err = httpd_resp_send_chunk(req, (const char *)jpeg_buf, jpeg_len);
            }
            if (err != ESP_OK) {
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(VOICE_HUB_CAMERA_WEB_FRAME_MS));
    }

    (void)httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(jpeg_buf);
    return ESP_OK;
}

static esp_err_t web_api_camera_resume_post(httpd_req_t *req)
{
    (void)req;
    web_pages_web_camera_resume();
    return web_send_json(req, "{\"ok\":true}");
}

static esp_err_t web_api_camera_view_get(httpd_req_t *req)
{
    voice_hub_camera_view_t view;
    char                    body[160];

    voice_hub_camera_view_get(&view);
    (void)snprintf(body,
                   sizeof(body),
                   "{\"ok\":true,\"rotate\":%u,\"flip_vertical\":%s,\"flip_horizontal\":%s}",
                   (unsigned)view.rotate_deg,
                   (view.flip_vertical != FALSE) ? "true" : "false",
                   (view.flip_horizontal != FALSE) ? "true" : "false");
    return web_send_json(req, body);
}

static esp_err_t web_api_camera_rotate_post(httpd_req_t *req)
{
    voice_hub_camera_view_t view;
    char                    body[160];

    (void)req;

    if (voice_hub_camera_view_rotate_cw() != STATUS_OK) {
        return web_send_json(req, "{\"ok\":false,\"error\":\"rotate_failed\"}");
    }

    voice_hub_camera_view_get(&view);
    (void)snprintf(body,
                   sizeof(body),
                   "{\"ok\":true,\"rotate\":%u,\"flip_vertical\":%s,\"flip_horizontal\":%s}",
                   (unsigned)view.rotate_deg,
                   (view.flip_vertical != FALSE) ? "true" : "false",
                   (view.flip_horizontal != FALSE) ? "true" : "false");
    return web_send_json(req, body);
}

#endif /* VOICE_HUB_ENABLE_CAMERA && VOICE_HUB_ENABLE_WIFI_WEB */

esp_err_t web_pages_register(httpd_handle_t server)
{
    httpd_uri_t status = {
        .uri     = "/api/voice_hub/status",
        .method  = HTTP_GET,
        .handler = web_api_status_get,
    };
    httpd_uri_t capture = {
        .uri     = "/api/voice_hub/capture",
        .method  = HTTP_POST,
        .handler = web_api_capture_post,
    };

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    web_ctrl_wifi_set_prepare_hook(web_pages_wifi_prepare_hook);

    {
        httpd_uri_t favicon = {
            .uri     = "/favicon.ico",
            .method  = HTTP_GET,
            .handler = web_favicon_get_handler,
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &favicon));
    }

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &capture));

#if VOICE_HUB_ENABLE_CAMERA && VOICE_HUB_ENABLE_WIFI_WEB
    {
        httpd_uri_t camera_jpeg = {
            .uri     = "/api/voice_hub/camera.jpg",
            .method  = HTTP_GET,
            .handler = web_api_camera_jpeg_get,
        };
        httpd_uri_t camera_mjpg = {
            .uri     = "/api/voice_hub/camera.mjpg",
            .method  = HTTP_GET,
            .handler = web_api_camera_mjpg_get,
        };
        httpd_uri_t camera_resume = {
            .uri     = "/api/voice_hub/camera_resume",
            .method  = HTTP_POST,
            .handler = web_api_camera_resume_post,
        };
        httpd_uri_t camera_view = {
            .uri     = "/api/voice_hub/camera_view",
            .method  = HTTP_GET,
            .handler = web_api_camera_view_get,
        };
        httpd_uri_t camera_rotate = {
            .uri     = "/api/voice_hub/camera_rotate",
            .method  = HTTP_POST,
            .handler = web_api_camera_rotate_post,
        };

        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &camera_jpeg));
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &camera_mjpg));
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &camera_resume));
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &camera_view));
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &camera_rotate));
    }
#endif

    return ESP_OK;
}
