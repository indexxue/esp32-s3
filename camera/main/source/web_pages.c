/**
 * @file web_pages.c
 * @brief camera HTTP 页面与 REST：根页面、状态、JPEG 快照、MJPEG 流。
 */

#include "web_pages.h"

#include "camera_model.h"
#include "camera_sensor.h"
#include "servo_ctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "log.h"
#include "net_wifi.h"
#include "nvs.h"
#include "web_ctrl_wifi_api.h"

#ifndef CAMERA_APP_COLLECT_MODE
#define CAMERA_APP_COLLECT_MODE 0
#endif
#ifndef CAMERA_APP_CALIB_MODE
#define CAMERA_APP_CALIB_MODE 0
#endif

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char preview_html_start[] asm("_binary_preview_html_start");
extern const char preview_html_end[] asm("_binary_preview_html_end");

static const char *TAG = "web_pages";

/** 与 sensor JPEG cache 对齐，避免大帧 MJPEG 静默丢帧。 */
#define CAMERA_WEB_JPEG_BUF_CAP (49152U)
#define CAMERA_WEB_MJPEG_BOUNDARY "frame"
#define CAMERA_WEB_MJPEG_MAX_FRAMES (0U) /* 0 = 直到客户端断开 */

#if CAMERA_APP_CALIB_MODE
static volatile bool s_web_camera_paused = true; /* 校准固件默认关图传，释放 httpd 槽 */
#else
static volatile bool s_web_camera_paused;
#endif
static uint8_t            *s_web_jpeg_buf;
static SemaphoreHandle_t   s_web_jpeg_mtx;
static volatile bool       s_mjpeg_busy;

/** 校准草稿（网页捕获后 save 才落 NVS）。 */
static nvs_servo_calib_t s_servo_calib_draft;
static bool              s_servo_calib_draft_ready;

/** 停 MJPEG 长连接并最多等 ~1s，释放 SoftAP httpd 槽。 */
static void web_pages_pause_stream(void)
{
    unsigned i;

    s_web_camera_paused = true;
    for (i = 0U; (i < 40U) && s_mjpeg_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void web_pages_wifi_pause_hook(void)
{
    web_pages_pause_stream();
}

static void web_pages_wifi_prepare_hook(void)
{
    web_pages_pause_stream();
    camera_sensor_prepare_for_reboot();
}

static status_t web_jpeg_buf_ensure(void)
{
    if (s_web_jpeg_buf != NULL) {
        return STATUS_OK;
    }
    s_web_jpeg_buf = (uint8_t *)heap_caps_malloc(CAMERA_WEB_JPEG_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_web_jpeg_buf == NULL) {
        s_web_jpeg_buf = (uint8_t *)heap_caps_malloc(CAMERA_WEB_JPEG_BUF_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_web_jpeg_buf == NULL) {
        return STATUS_NO_MEM;
    }
    if (s_web_jpeg_mtx == NULL) {
        s_web_jpeg_mtx = xSemaphoreCreateMutex();
        if (s_web_jpeg_mtx == NULL) {
            heap_caps_free(s_web_jpeg_buf);
            s_web_jpeg_buf = NULL;
            return STATUS_NO_MEM;
        }
    }
    return STATUS_OK;
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
#if CAMERA_APP_CALIB_MODE
    /* 校准固件：放行，避免 getpeername/子网误判导致页面永远「加载中」且无下发。 */
    (void)req;
    return true;
#else
    return net_wifi_http_peer_on_local_subnet(httpd_req_to_sockfd(req));
#endif
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
#if CAMERA_APP_CALIB_MODE
    LOG_INFO("web GET / (calib index)");
#endif
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    (void)httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, index_html_start, len);
}

/** 独占图传页：全屏 MJPEG，进入时关 LCD，离开/暂停后释放通道。 */
static esp_err_t web_pages_preview_get_handler(httpd_req_t *req)
{
    const size_t len = (size_t)(preview_html_end - preview_html_start);

    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    (void)httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, preview_html_start, len);
}

static esp_err_t web_favicon_get_handler(httpd_req_t *req)
{
    (void)req;
    (void)httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0U);
}

static esp_err_t web_api_status_get(httpd_req_t *req)
{
    char body[640];

    (void)snprintf(body,
                   sizeof(body),
                   "{\"product\":\"camera\",\"camera\":%s,\"width\":%u,\"height\":%u,"
                   "\"quality\":%u,\"poll_ms\":%u,\"grayscale\":%u,\"zoom\":%u,"
                   "\"rotate\":%u,\"flip_v\":%u,\"flip_h\":%u,"
                   "\"cfg_width\":%u,\"cfg_height\":%u,"
                   "\"frames\":%u,\"lcd_blit\":%u,\"paused\":%s,"
                   "\"mjpeg_busy\":%s,\"exclusive\":%s,"
                   "\"collect_mode\":%s,\"detect\":%s,"
                   "\"stream\":\"/api/camera/stream.mjpg\","
                   "\"preview\":\"/preview.html\","
                   "\"sizes\":[\"240x240\"]}",
                   (camera_sensor_is_ready() != FALSE) ? "true" : "false",
                   (unsigned)camera_sensor_get_snapshot_width(),
                   (unsigned)camera_sensor_get_snapshot_height(),
                   (unsigned)camera_sensor_get_jpeg_quality(),
                   (unsigned)camera_sensor_get_web_poll_ms(),
                   (unsigned)camera_sensor_get_grayscale(),
                   (unsigned)camera_sensor_get_zoom(),
                   (unsigned)camera_sensor_get_img_rotate(),
                   (unsigned)camera_sensor_get_flip_v(),
                   (unsigned)camera_sensor_get_flip_h(),
                   (unsigned)camera_sensor_get_web_width(),
                   (unsigned)camera_sensor_get_web_height(),
                   (unsigned)camera_sensor_get_frame_count(),
                   (unsigned)camera_sensor_get_lcd_blit_count(),
                   s_web_camera_paused ? "true" : "false",
                   s_mjpeg_busy ? "true" : "false",
                   (camera_sensor_web_stream_active() != FALSE) ? "true" : "false",
                   CAMERA_APP_COLLECT_MODE ? "true" : "false",
                   ((camera_model_is_ready() != FALSE) && (camera_model_is_enabled() != FALSE))
                       ? "true"
                       : "false");
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

/** 单帧 JPEG：拍照下载 / 兼容旧轮询。使用静态缓冲，避免每请求 malloc。 */
static esp_err_t web_api_camera_jpeg_get(httpd_req_t *req)
{
    uint32_t jpeg_len = 0U;

    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
    if (web_camera_check_ready(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (web_jpeg_buf_ensure() != STATUS_OK) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_memory\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (xSemaphoreTake(s_web_jpeg_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"busy\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (camera_sensor_snapshot_jpeg(s_web_jpeg_buf, CAMERA_WEB_JPEG_BUF_CAP, &jpeg_len) != STATUS_OK) {
        (void)xSemaphoreGive(s_web_jpeg_mtx);
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_frame\"}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_type(req, "image/jpeg");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_send(req, (const char *)s_web_jpeg_buf, (size_t)jpeg_len);
    (void)xSemaphoreGive(s_web_jpeg_mtx);
    return ESP_OK;
}

/** multipart/x-mixed-replace MJPEG：浏览器 <img src> 直接播流。 */
static esp_err_t web_api_camera_mjpeg_get(httpd_req_t *req)
{
    uint32_t seq = 0U;
    uint32_t frames = 0U;
    char     part_hdr[128];
    esp_err_t err = ESP_OK;
    TickType_t last_send_tick = 0;
    /* ~6fps：预览常开时把 CPU/带宽让给检测，避免球快时大量空框。 */
    const TickType_t min_gap = pdMS_TO_TICKS(160);

    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
#if CAMERA_APP_CALIB_MODE
    (void)seq;
    (void)frames;
    (void)part_hdr;
    (void)last_send_tick;
    (void)min_gap;
    (void)httpd_resp_set_status(req, "503 Service Unavailable");
    return web_send_json(req, "{\"ok\":false,\"error\":\"stream_disabled_calib\",\"msg\":\"calib mode: stream off\"}");
#endif
    if (web_camera_check_ready(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (s_mjpeg_busy) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"stream_busy\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (web_jpeg_buf_ensure() != STATUS_OK) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_memory\"}", HTTPD_RESP_USE_STRLEN);
    }

    s_mjpeg_busy = true;
    camera_sensor_web_stream_enter();

    (void)httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=" CAMERA_WEB_MJPEG_BOUNDARY);
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    (void)httpd_resp_set_hdr(req, "Pragma", "no-cache");
    (void)httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (!s_web_camera_paused) {
        uint32_t jpeg_len = 0U;
        int      hdr_len;
        TickType_t now;

        /* 短超时轮询，便于 camera_pause 后尽快退出占槽。 */
        if (camera_sensor_wait_jpeg_seq(&seq, 200U) != STATUS_OK) {
            continue;
        }

        now = xTaskGetTickCount();
        if ((last_send_tick != 0) && ((now - last_send_tick) < min_gap)) {
            vTaskDelay(min_gap - (now - last_send_tick));
        }

        if (xSemaphoreTake(s_web_jpeg_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (camera_sensor_copy_jpeg_cache(s_web_jpeg_buf, CAMERA_WEB_JPEG_BUF_CAP, &jpeg_len) != STATUS_OK) {
            (void)xSemaphoreGive(s_web_jpeg_mtx);
            continue;
        }

        hdr_len = snprintf(part_hdr,
                           sizeof(part_hdr),
                           "--" CAMERA_WEB_MJPEG_BOUNDARY "\r\n"
                           "Content-Type: image/jpeg\r\n"
                           "Content-Length: %u\r\n"
                           "\r\n",
                           (unsigned)jpeg_len);
        if (hdr_len <= 0) {
            (void)xSemaphoreGive(s_web_jpeg_mtx);
            err = ESP_FAIL;
            break;
        }

        err = httpd_resp_send_chunk(req, part_hdr, (ssize_t)hdr_len);
        if (err != ESP_OK) {
            (void)xSemaphoreGive(s_web_jpeg_mtx);
            break;
        }
        err = httpd_resp_send_chunk(req, (const char *)s_web_jpeg_buf, (ssize_t)jpeg_len);
        (void)xSemaphoreGive(s_web_jpeg_mtx);
        if (err != ESP_OK) {
            break;
        }
        err = httpd_resp_send_chunk(req, "\r\n", 2);
        if (err != ESP_OK) {
            break;
        }

        last_send_tick = xTaskGetTickCount();
        frames++;
#if CAMERA_WEB_MJPEG_MAX_FRAMES > 0
        if (frames >= CAMERA_WEB_MJPEG_MAX_FRAMES) {
            break;
        }
#endif
    }

    (void)httpd_resp_send_chunk(req, NULL, 0);
    camera_sensor_web_stream_leave();
    s_mjpeg_busy = false;
    /* 对端关页/切后台属正常，降为 DEBUG，避免刷 WARN。 */
    if ((err == ESP_OK) || (err == ESP_ERR_HTTPD_RESP_SEND)) {
        ESP_LOGD(TAG, "mjpeg end frames=%u err=%s", (unsigned)frames, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "mjpeg end frames=%u err=%s", (unsigned)frames, esp_err_to_name(err));
    }
    return ESP_OK;
}

static esp_err_t web_api_camera_pause_post(httpd_req_t *req)
{
    char json[96];
    int  n;

    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
    web_pages_pause_stream();
    n = snprintf(json,
                 sizeof(json),
                 "{\"ok\":true,\"paused\":true,\"mjpeg_busy\":%s}",
                 s_mjpeg_busy ? "true" : "false");
    if ((n <= 0) || ((size_t)n >= sizeof(json))) {
        return web_send_json(req, "{\"ok\":true,\"paused\":true}");
    }
    return web_send_json(req, json);
}

static esp_err_t web_api_camera_resume_post(httpd_req_t *req)
{
    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
#if CAMERA_APP_CALIB_MODE
    /* 校准固件禁止恢复图传，避免再次占满 SoftAP httpd 槽。 */
    return web_send_json(req, "{\"ok\":false,\"error\":\"stream_disabled_calib\",\"paused\":true}");
#else
    s_web_camera_paused = false;
    return web_send_json(req, "{\"ok\":true,\"paused\":false}");
#endif
}

static bool web_parse_u16_after_key(const char *body, const char *key, uint16_t *out)
{
    const char *p = strstr(body, key);
    if ((p == NULL) || (out == NULL)) {
        return false;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    *out = (uint16_t)strtoul(p + 1, NULL, 10);
    return true;
}

static bool web_parse_f32_after_key(const char *body, const char *key, float *out)
{
    const char *p = strstr(body, key);
    char       *end = NULL;
    float       v;

    if ((p == NULL) || (out == NULL)) {
        return false;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    v = strtof(p + 1, &end);
    if (end == (p + 1)) {
        return false;
    }
    *out = v;
    return true;
}

static esp_err_t web_api_servo_status_json(httpd_req_t *req)
{
    char            body[384];
    float           pan  = 0.0f;
    float           tilt = 0.0f;
    uint16_t        pan_us  = 0U;
    uint16_t        tilt_us = 0U;
    servo_limits_t  lim;
    bool            ready;

    (void)memset(&lim, 0, sizeof(lim));
    (void)servo_get_limits(&lim);

    ready = (servo_is_ready() != FALSE);
    if (ready) {
        (void)servo_get_angle(SERVO_CH_PAN, &pan);
        (void)servo_get_angle(SERVO_CH_TILT, &tilt);
        (void)servo_get_pulse_us(SERVO_CH_PAN, &pan_us);
        (void)servo_get_pulse_us(SERVO_CH_TILT, &tilt_us);
    }

    (void)snprintf(body,
                   sizeof(body),
                   "{\"ok\":true,\"ready\":%s,\"pan_deg\":%.1f,\"tilt_deg\":%.1f,"
                   "\"pan_us\":%u,\"tilt_us\":%u,"
                   "\"pan_min\":%.1f,\"pan_max\":%.1f,\"tilt_min\":%.1f,\"tilt_max\":%.1f,"
                   "\"angle_max\":%.0f,\"center\":%.0f,\"pulse_min\":%u,\"pulse_max\":%u,"
                   "\"step_default\":5}",
                   ready ? "true" : "false",
                   (double)pan,
                   (double)tilt,
                   (unsigned)pan_us,
                   (unsigned)tilt_us,
                   (double)lim.pan_min_deg,
                   (double)lim.pan_max_deg,
                   (double)lim.tilt_min_deg,
                   (double)lim.tilt_max_deg,
                   (double)lim.angle_max_deg,
                   (double)lim.center_deg,
                   (unsigned)BOARD_SERVO_PULSE_MIN_US,
                   (unsigned)BOARD_SERVO_PULSE_MAX_US);
    return web_send_json(req, body);
}

static esp_err_t web_api_servo_status_get(httpd_req_t *req)
{
    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
    return web_api_servo_status_json(req);
}

/**
 * POST /api/servo
 * {"center":1}
 * {"pan":180} / {"tilt":180}              绝对角（0–angle_max，受软限位）
 * {"pan_delta":5} / {"tilt_delta":-5}     相对步进
 * {"pan_min":0,"pan_max":360,"tilt_min":60,"tilt_max":300}  改软限位
 * {"limits_reset":1}                      恢复 board.h 默认限位
 * {"pan_pulse":1500} / {"tilt_pulse":500} 原始脉宽 µs（绕过角度软限位，仍夹在 500–2500）
 */
static esp_err_t web_api_servo_post(httpd_req_t *req)
{
    char     body[256];
    int      received;
    float    v;
    float    pan_min;
    float    pan_max;
    float    tilt_min;
    float    tilt_max;
    uint16_t center16 = 0U;
    uint16_t reset16  = 0U;
    uint16_t pulse16  = 0U;
    bool     acted    = false;
    bool     lim_any  = false;
    const float *p_pan_min  = NULL;
    const float *p_pan_max  = NULL;
    const float *p_tilt_min = NULL;
    const float *p_tilt_max = NULL;
    status_t st       = STATUS_OK;

    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
    if (servo_is_ready() == FALSE) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        return web_send_json(req, "{\"ok\":false,\"error\":\"servo_not_ready\"}");
    }
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"ok\":false,\"error\":\"bad_body\"}");
    }

    received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"ok\":false,\"error\":\"bad_body\"}");
    }
    body[received] = '\0';

    if (web_parse_u16_after_key(body, "\"limits_reset\"", &reset16) && (reset16 != 0U)) {
        st    = servo_reset_limits();
        acted = true;
    }
    if (web_parse_f32_after_key(body, "\"pan_min\"", &pan_min)) {
        p_pan_min = &pan_min;
        lim_any   = true;
    }
    if (web_parse_f32_after_key(body, "\"pan_max\"", &pan_max)) {
        p_pan_max = &pan_max;
        lim_any   = true;
    }
    if (web_parse_f32_after_key(body, "\"tilt_min\"", &tilt_min)) {
        p_tilt_min = &tilt_min;
        lim_any    = true;
    }
    if (web_parse_f32_after_key(body, "\"tilt_max\"", &tilt_max)) {
        p_tilt_max = &tilt_max;
        lim_any    = true;
    }
    if (lim_any) {
        st = servo_set_limits(p_pan_min, p_pan_max, p_tilt_min, p_tilt_max);
        if (st == STATUS_INVALID_ARG) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_limits\"}");
        }
        acted = true;
    }

    if (web_parse_u16_after_key(body, "\"center\"", &center16) && (center16 != 0U)) {
        st    = servo_center_all();
        acted = true;
    }
    if (web_parse_f32_after_key(body, "\"pan_delta\"", &v)) {
        st    = servo_nudge(SERVO_CH_PAN, v);
        acted = true;
    }
    if (web_parse_f32_after_key(body, "\"tilt_delta\"", &v)) {
        st    = servo_nudge(SERVO_CH_TILT, v);
        acted = true;
    }
    /* 原始脉宽：绕过角度软限位，便于扫极限 */
    if (web_parse_u16_after_key(body, "\"pan_pulse\"", &pulse16)) {
        st    = servo_set_pulse_us(SERVO_CH_PAN, pulse16);
        acted = true;
    }
    if (web_parse_u16_after_key(body, "\"tilt_pulse\"", &pulse16)) {
        st    = servo_set_pulse_us(SERVO_CH_TILT, pulse16);
        acted = true;
    }
    /* 绝对角：更长键名已先处理；此处匹配 "pan": / "tilt": */
    if (web_parse_f32_after_key(body, "\"pan\":", &v)) {
        st    = servo_set_angle(SERVO_CH_PAN, v);
        acted = true;
    }
    if (web_parse_f32_after_key(body, "\"tilt\":", &v)) {
        st    = servo_set_angle(SERVO_CH_TILT, v);
        acted = true;
    }

    if (!acted) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"ok\":false,\"error\":\"no_action\"}");
    }
    if (st != STATUS_OK) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        return web_send_json(req, "{\"ok\":false,\"error\":\"servo_fail\"}");
    }
    return web_api_servo_status_json(req);
}

static void web_servo_calib_draft_ensure(void)
{
    if (s_servo_calib_draft_ready) {
        return;
    }
    if (!nvs_servo_calib_get(&s_servo_calib_draft)) {
        nvs_servo_calib_default(&s_servo_calib_draft);
    }
    s_servo_calib_draft_ready = true;
}

static servo_ch_t web_servo_calib_ch(void)
{
    web_servo_calib_draft_ensure();
    return (s_servo_calib_draft.channel == NVS_SERVO_CALIB_CH_TILT) ? SERVO_CH_TILT : SERVO_CH_PAN;
}

static const char *web_servo_calib_ch_name(void)
{
    return (web_servo_calib_ch() == SERVO_CH_TILT) ? "tilt" : "pan";
}

static nvs_servo_pose_t *web_servo_calib_pose_ptr(nvs_servo_calib_t *cal, uint8_t which)
{
    if (cal == NULL) {
        return NULL;
    }
    if (which == NVS_SERVO_CALIB_VALID_LEFT) {
        return &cal->left;
    }
    if (which == NVS_SERVO_CALIB_VALID_CENTER) {
        return &cal->center;
    }
    if (which == NVS_SERVO_CALIB_VALID_RIGHT) {
        return &cal->right;
    }
    return NULL;
}

static const char *web_pose_name(uint8_t mask)
{
    if (mask == NVS_SERVO_CALIB_VALID_LEFT) {
        return "left";
    }
    if (mask == NVS_SERVO_CALIB_VALID_CENTER) {
        return "center";
    }
    if (mask == NVS_SERVO_CALIB_VALID_RIGHT) {
        return "right";
    }
    return "?";
}

static bool web_parse_pose_mask(const char *body, uint8_t *out_mask)
{
    const char *p;

    if ((body == NULL) || (out_mask == NULL)) {
        return false;
    }
    p = strstr(body, "\"pose\"");
    if (p == NULL) {
        return false;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    if (strstr(p, "left") != NULL) {
        *out_mask = NVS_SERVO_CALIB_VALID_LEFT;
        return true;
    }
    if (strstr(p, "center") != NULL) {
        *out_mask = NVS_SERVO_CALIB_VALID_CENTER;
        return true;
    }
    if (strstr(p, "right") != NULL) {
        *out_mask = NVS_SERVO_CALIB_VALID_RIGHT;
        return true;
    }
    return false;
}

static int web_servo_calib_pose_json(char *dst, size_t cap, const char *name, const nvs_servo_pose_t *pose,
                                    bool valid)
{
    float    us_per_deg;
    float    eff;
    uint16_t eff_us;

    us_per_deg = (float)(BOARD_SERVO_PULSE_MAX_US - BOARD_SERVO_PULSE_MIN_US) /
                 (float)BOARD_SERVO_ANGLE_MAX_DEG;
    eff = (float)pose->pulse_us + (pose->offset_deg * us_per_deg);
    if (eff < (float)BOARD_SERVO_PULSE_MIN_US) {
        eff = (float)BOARD_SERVO_PULSE_MIN_US;
    }
    if (eff > (float)BOARD_SERVO_PULSE_MAX_US) {
        eff = (float)BOARD_SERVO_PULSE_MAX_US;
    }
    eff_us = (uint16_t)(eff + 0.5f);

    return snprintf(dst,
                    cap,
                    "\"%s\":{\"valid\":%s,\"angle_deg\":%.1f,\"offset_deg\":%.2f,\"pulse_us\":%u,"
                    "\"eff_us\":%u}",
                    name,
                    valid ? "true" : "false",
                    (double)pose->angle_deg,
                    (double)pose->offset_deg,
                    (unsigned)pose->pulse_us,
                    (unsigned)eff_us);
}

static void web_servo_calib_order_hint(const nvs_servo_calib_t *cal, char *order, size_t order_cap,
                                       char *hint, size_t hint_cap)
{
    float    us_per_deg;
    float    le;
    float    ce;
    float    re;
    uint16_t l_us;
    uint16_t c_us;
    uint16_t r_us;

    if ((cal == NULL) || (order == NULL) || (hint == NULL)) {
        return;
    }
    if ((cal->valid_mask & NVS_SERVO_CALIB_VALID_ALL) != NVS_SERVO_CALIB_VALID_ALL) {
        (void)snprintf(order, order_cap, "incomplete");
        (void)snprintf(hint, hint_cap, "need L+C+R in NVS");
        return;
    }

    us_per_deg = (float)(BOARD_SERVO_PULSE_MAX_US - BOARD_SERVO_PULSE_MIN_US) /
                 (float)BOARD_SERVO_ANGLE_MAX_DEG;
    le   = (float)cal->left.pulse_us + (cal->left.offset_deg * us_per_deg);
    ce   = (float)cal->center.pulse_us + (cal->center.offset_deg * us_per_deg);
    re   = (float)cal->right.pulse_us + (cal->right.offset_deg * us_per_deg);
    l_us = (uint16_t)(le + 0.5f);
    c_us = (uint16_t)(ce + 0.5f);
    r_us = (uint16_t)(re + 0.5f);

    if ((l_us < c_us) && (c_us < r_us)) {
        (void)snprintf(order, order_cap, "L<C<R");
        (void)snprintf(hint, hint_cap, "ok ascending; detect INVERT=0 means ball-left tips RIGHT");
    } else if ((l_us > c_us) && (c_us > r_us)) {
        (void)snprintf(order, order_cap, "L>C>R");
        (void)snprintf(hint, hint_cap, "descending; re-calib for L<C<R or try INVERT=1");
    } else {
        (void)snprintf(order, order_cap, "non_monotonic");
        (void)snprintf(hint, hint_cap, "BAD: re-calib physical left=Left mid=Center right=Right");
    }
}

static esp_err_t web_api_servo_calib_json_msg(httpd_req_t *req, const char *msg)
{
    char     body[1400];
    char     left_js[200];
    char     center_js[200];
    char     right_js[200];
    char     nvs_left_js[200];
    char     nvs_center_js[200];
    char     nvs_right_js[200];
    char     msg_js[96];
    char     order[24];
    char     hint[128];
    float    pan      = 0.0f;
    float    tilt     = 0.0f;
    float    cur_deg  = 0.0f;
    uint16_t pan_us   = 0U;
    uint16_t tilt_us  = 0U;
    uint16_t cur_us   = 0U;
    bool     ready;
    bool     nvs_ok;
    nvs_servo_calib_t nvs_cal;
    servo_ch_t        ch;

    web_servo_calib_draft_ensure();
    ch = web_servo_calib_ch();

    ready = (servo_is_ready() != FALSE);
    if (ready) {
        (void)servo_get_angle(SERVO_CH_PAN, &pan);
        (void)servo_get_angle(SERVO_CH_TILT, &tilt);
        (void)servo_get_pulse_us(SERVO_CH_PAN, &pan_us);
        (void)servo_get_pulse_us(SERVO_CH_TILT, &tilt_us);
        (void)servo_get_angle(ch, &cur_deg);
        (void)servo_get_pulse_us(ch, &cur_us);
    }

    nvs_ok = nvs_servo_calib_get(&nvs_cal);
    (void)web_servo_calib_pose_json(left_js, sizeof(left_js), "left", &s_servo_calib_draft.left,
                                    (s_servo_calib_draft.valid_mask & NVS_SERVO_CALIB_VALID_LEFT) != 0U);
    (void)web_servo_calib_pose_json(center_js, sizeof(center_js), "center", &s_servo_calib_draft.center,
                                    (s_servo_calib_draft.valid_mask & NVS_SERVO_CALIB_VALID_CENTER) != 0U);
    (void)web_servo_calib_pose_json(right_js, sizeof(right_js), "right", &s_servo_calib_draft.right,
                                    (s_servo_calib_draft.valid_mask & NVS_SERVO_CALIB_VALID_RIGHT) != 0U);

    order[0] = '\0';
    hint[0]  = '\0';
    nvs_left_js[0] = nvs_center_js[0] = nvs_right_js[0] = '\0';
    if (nvs_ok) {
        (void)web_servo_calib_pose_json(nvs_left_js, sizeof(nvs_left_js), "left", &nvs_cal.left,
                                        (nvs_cal.valid_mask & NVS_SERVO_CALIB_VALID_LEFT) != 0U);
        (void)web_servo_calib_pose_json(nvs_center_js, sizeof(nvs_center_js), "center", &nvs_cal.center,
                                        (nvs_cal.valid_mask & NVS_SERVO_CALIB_VALID_CENTER) != 0U);
        (void)web_servo_calib_pose_json(nvs_right_js, sizeof(nvs_right_js), "right", &nvs_cal.right,
                                        (nvs_cal.valid_mask & NVS_SERVO_CALIB_VALID_RIGHT) != 0U);
        web_servo_calib_order_hint(&nvs_cal, order, sizeof(order), hint, sizeof(hint));
    } else {
        (void)snprintf(order, sizeof(order), "empty");
        (void)snprintf(hint, sizeof(hint), "NVS empty — calibrate L/C/R");
    }

    if ((msg != NULL) && (msg[0] != '\0')) {
        (void)snprintf(msg_js, sizeof(msg_js), "%s", msg);
    } else {
        msg_js[0] = '\0';
    }

    (void)snprintf(body,
                   sizeof(body),
                   "{\"ok\":true,\"ready\":%s,\"channel\":\"%s\",\"nvs_present\":%s,"
                   "\"angle_deg\":%.1f,\"pulse_us\":%u,"
                   "\"pan_deg\":%.1f,\"pan_us\":%u,\"tilt_deg\":%.1f,\"tilt_us\":%u,"
                   "\"valid_mask\":%u,\"draft\":{%s,%s,%s},"
                   "\"nvs\":{%s,%s,%s},"
                   "\"check\":{\"order\":\"%s\",\"hint\":\"%s\"},"
                   "\"angle_max\":%d,\"center\":%d,\"pulse_min\":%u,\"pulse_max\":%u,"
                   "\"msg\":\"%s\"}",
                   ready ? "true" : "false",
                   web_servo_calib_ch_name(),
                   nvs_ok ? "true" : "false",
                   (double)cur_deg,
                   (unsigned)cur_us,
                   (double)pan,
                   (unsigned)pan_us,
                   (double)tilt,
                   (unsigned)tilt_us,
                   (unsigned)s_servo_calib_draft.valid_mask,
                   left_js,
                   center_js,
                   right_js,
                   nvs_ok ? nvs_left_js : "\"left\":null",
                   nvs_ok ? nvs_center_js : "\"center\":null",
                   nvs_ok ? nvs_right_js : "\"right\":null",
                   order,
                   hint,
                   BOARD_SERVO_ANGLE_MAX_DEG,
                   BOARD_SERVO_CENTER_DEG,
                   (unsigned)BOARD_SERVO_PULSE_MIN_US,
                   (unsigned)BOARD_SERVO_PULSE_MAX_US,
                   msg_js);
    return web_send_json(req, body);
}

static esp_err_t web_api_servo_calib_json(httpd_req_t *req)
{
    return web_api_servo_calib_json_msg(req, "");
}

static esp_err_t web_api_app_mode_get(httpd_req_t *req)
{
    const char *mode;

    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
#if CAMERA_APP_CALIB_MODE
    mode = "calib";
#elif CAMERA_APP_COLLECT_MODE
    mode = "collect";
#else
    mode = "detect";
#endif
    {
        char body[64];
        (void)snprintf(body, sizeof(body), "{\"ok\":true,\"mode\":\"%s\"}", mode);
        return web_send_json(req, body);
    }
}

static esp_err_t web_api_servo_calib_get(httpd_req_t *req)
{
    if (!web_local_peer_allowed(req)) {
        LOG_WARN("web calib GET forbidden (not local subnet)");
        return web_sensitive_forbidden(req);
    }
    /* 服务端强制停 MJPEG，不依赖浏览器先 POST pause（槽满时 pause 也进不来）。 */
    web_pages_pause_stream();
    LOG_INFO("web calib GET");
    return web_api_servo_calib_json(req);
}

/**
 * POST /api/servo/calib
 * {"action":"set_channel","channel":"pan|tilt"}
 * {"action":"nudge","delta":5}
 * {"action":"set_angle","angle":180}
 * {"action":"set_pulse","pulse":1500}
 * {"action":"capture","pose":"left|center|right"}
 * {"action":"set_offset","pose":"left","offset":1.5}
 * {"action":"goto","pose":"center"}
 * {"action":"save_pose","pose":"left"}  仅写入该姿态到 NVS
 * {"action":"save"} / {"action":"load"} / {"action":"reset","delete_nvs":1}
 */
static esp_err_t web_api_servo_calib_post(httpd_req_t *req)
{
    char     body[256];
    int      received;
    uint8_t  pose_mask = 0U;
    float    v;
    uint16_t pulse16   = 0U;
    uint16_t del_nvs   = 0U;
    status_t st        = STATUS_OK;
    nvs_servo_pose_t *pose;
    servo_ch_t        ch;
    char              msg[80];

    msg[0] = '\0';

    if (!web_local_peer_allowed(req)) {
        LOG_WARN("web calib POST forbidden (not local subnet)");
        return web_sensitive_forbidden(req);
    }

    /* 一进校准 API 就释放图传槽，保证后续舵机动作可达。 */
    web_pages_pause_stream();
    LOG_INFO("web calib POST len=%d", (int)req->content_len);

    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
        LOG_WARN("web calib bad_body len=%d", (int)req->content_len);
        (void)httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"ok\":false,\"error\":\"bad_body\"}");
    }

    received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        LOG_WARN("web calib recv fail");
        (void)httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"ok\":false,\"error\":\"bad_body\"}");
    }
    body[received] = '\0';
    LOG_INFO("web calib body: %s", body);

    web_servo_calib_draft_ensure();
    ch = web_servo_calib_ch();

    if (strstr(body, "\"action\":\"set_channel\"") != NULL) {
        if (strstr(body, "\"tilt\"") != NULL) {
            s_servo_calib_draft.channel = NVS_SERVO_CALIB_CH_TILT;
        } else if (strstr(body, "\"pan\"") != NULL) {
            s_servo_calib_draft.channel = NVS_SERVO_CALIB_CH_PAN;
        } else {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_channel\"}");
        }
        ch = web_servo_calib_ch();
        (void)snprintf(msg, sizeof(msg), "channel=%s GPIO%d", web_servo_calib_ch_name(),
                       (ch == SERVO_CH_TILT) ? BOARD_SERVO_TILT_PIN : BOARD_SERVO_PAN_PIN);
        LOG_INFO("web calib: %s", msg);
        return web_api_servo_calib_json_msg(req, msg);
    }

    if (strstr(body, "\"action\":\"load\"") != NULL) {
        if (!nvs_servo_calib_get(&s_servo_calib_draft)) {
            nvs_servo_calib_default(&s_servo_calib_draft);
            (void)httpd_resp_set_status(req, "404 Not Found");
            return web_send_json(req, "{\"ok\":false,\"error\":\"nvs_empty\",\"msg\":\"NVS empty\"}");
        }
        return web_api_servo_calib_json_msg(req, "loaded from NVS");
    }

    if (strstr(body, "\"action\":\"reset\"") != NULL) {
        nvs_servo_calib_default(&s_servo_calib_draft);
        if (web_parse_u16_after_key(body, "\"delete_nvs\"", &del_nvs) && (del_nvs != 0U)) {
            (void)nvs_servo_calib_delete();
            return web_api_servo_calib_json_msg(req, "draft+NVS cleared");
        }
        return web_api_servo_calib_json_msg(req, "draft reset");
    }

    if (strstr(body, "\"action\":\"save_pose\"") != NULL) {
        nvs_servo_calib_t nvs_cal;
        nvs_servo_pose_t *dst;

        if (!web_parse_pose_mask(body, &pose_mask)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_pose\"}");
        }
        if ((s_servo_calib_draft.valid_mask & pose_mask) == 0U) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"pose_not_captured\",\"msg\":\"capture first\"}");
        }
        pose = web_servo_calib_pose_ptr(&s_servo_calib_draft, pose_mask);
        if (pose == NULL) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_pose\"}");
        }
        if (!nvs_servo_calib_get(&nvs_cal)) {
            nvs_servo_calib_default(&nvs_cal);
        }
        nvs_cal.channel = s_servo_calib_draft.channel;
        dst             = web_servo_calib_pose_ptr(&nvs_cal, pose_mask);
        if (dst == NULL) {
            (void)httpd_resp_set_status(req, "500 Internal Server Error");
            return web_send_json(req, "{\"ok\":false,\"error\":\"save_failed\"}");
        }
        *dst = *pose;
        nvs_cal.valid_mask |= pose_mask;
        if (!nvs_servo_calib_validate(&nvs_cal) || !nvs_servo_calib_set(&nvs_cal)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"save_failed\"}");
        }
        s_servo_calib_draft.valid_mask |= pose_mask;
        (void)snprintf(msg, sizeof(msg), "NVS saved %s (ch=%s)", web_pose_name(pose_mask),
                       web_servo_calib_ch_name());
        return web_api_servo_calib_json_msg(req, msg);
    }

    if (strstr(body, "\"action\":\"save\"") != NULL) {
        if (!nvs_servo_calib_validate(&s_servo_calib_draft) || !nvs_servo_calib_set(&s_servo_calib_draft)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"save_failed\"}");
        }
        return web_api_servo_calib_json_msg(req, "NVS saved all poses");
    }

    if (servo_is_ready() == FALSE) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        return web_send_json(req, "{\"ok\":false,\"error\":\"servo_not_ready\",\"msg\":\"servo not ready\"}");
    }

    if (strstr(body, "\"action\":\"nudge\"") != NULL) {
        if (!web_parse_f32_after_key(body, "\"delta\"", &v)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_delta\"}");
        }
        st = servo_nudge(ch, v);
        (void)snprintf(msg, sizeof(msg), "nudge %s %+.1f deg", web_servo_calib_ch_name(), (double)v);
    } else if (strstr(body, "\"action\":\"set_angle\"") != NULL) {
        if (!web_parse_f32_after_key(body, "\"angle\"", &v)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_angle\"}");
        }
        st = servo_set_angle(ch, v);
        (void)snprintf(msg, sizeof(msg), "set_angle %s=%.1f", web_servo_calib_ch_name(), (double)v);
    } else if (strstr(body, "\"action\":\"set_pulse\"") != NULL) {
        if (!web_parse_u16_after_key(body, "\"pulse\"", &pulse16)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_pulse\"}");
        }
        st = servo_set_pulse_us(ch, pulse16);
        (void)snprintf(msg, sizeof(msg), "set_pulse %s=%u us", web_servo_calib_ch_name(), (unsigned)pulse16);
    } else if (strstr(body, "\"action\":\"capture\"") != NULL) {
        float    ang = 0.0f;
        uint16_t us  = 0U;

        if (!web_parse_pose_mask(body, &pose_mask)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_pose\"}");
        }
        pose = web_servo_calib_pose_ptr(&s_servo_calib_draft, pose_mask);
        if ((pose == NULL) || (servo_get_angle(ch, &ang) != STATUS_OK) ||
            (servo_get_pulse_us(ch, &us) != STATUS_OK)) {
            (void)httpd_resp_set_status(req, "500 Internal Server Error");
            return web_send_json(req, "{\"ok\":false,\"error\":\"capture_fail\"}");
        }
        pose->angle_deg  = ang;
        pose->pulse_us   = us;
        pose->offset_deg = 0.0f;
        s_servo_calib_draft.valid_mask |= pose_mask;
        st = STATUS_OK;
        (void)snprintf(msg, sizeof(msg), "captured %s %.1fdeg/%uus", web_pose_name(pose_mask), (double)ang,
                       (unsigned)us);
    } else if (strstr(body, "\"action\":\"set_offset\"") != NULL) {
        if (!web_parse_pose_mask(body, &pose_mask) || !web_parse_f32_after_key(body, "\"offset\"", &v)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_offset\"}");
        }
        pose = web_servo_calib_pose_ptr(&s_servo_calib_draft, pose_mask);
        if (pose == NULL) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_pose\"}");
        }
        pose->offset_deg = v;
        s_servo_calib_draft.valid_mask |= pose_mask;
        st = servo_apply_pose(ch, pose->angle_deg, pose->offset_deg, pose->pulse_us);
        (void)snprintf(msg, sizeof(msg), "offset %s=%.2f applied", web_pose_name(pose_mask), (double)v);
    } else if (strstr(body, "\"action\":\"goto\"") != NULL) {
        if (!web_parse_pose_mask(body, &pose_mask)) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_pose\"}");
        }
        pose = web_servo_calib_pose_ptr(&s_servo_calib_draft, pose_mask);
        if (pose == NULL) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"bad_pose\"}");
        }
        if ((s_servo_calib_draft.valid_mask & pose_mask) == 0U) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            return web_send_json(req, "{\"ok\":false,\"error\":\"pose_not_captured\",\"msg\":\"capture first\"}");
        }
        st = servo_apply_pose(ch, pose->angle_deg, pose->offset_deg, pose->pulse_us);
        (void)snprintf(msg, sizeof(msg), "goto %s", web_pose_name(pose_mask));
    } else {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"ok\":false,\"error\":\"no_action\"}");
    }

    if (st != STATUS_OK) {
        LOG_WARN("web calib servo_fail action body was logged above");
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        return web_send_json(req, "{\"ok\":false,\"error\":\"servo_fail\",\"msg\":\"servo_fail\"}");
    }
    LOG_INFO("web calib: %s", msg);
    return web_api_servo_calib_json_msg(req, msg);
}

/**
 * POST /api/camera/config
 * {"size":"320x240","quality":55,"grayscale":0,"zoom":1,"rotate":0,"flip_v":1,"flip_h":0,"persist":1}
 * rotate: 0/1/2/3 → 0°/90°/180°/270°（图像旋转，非屏幕驱动）
 * persist: 1=写入 NVS（默认），0=仅运行时预览
 */
static esp_err_t web_api_camera_config_post(httpd_req_t *req)
{
    char                  body[256];
    int                   received;
    nvs_camera_settings_t st;
    uint16_t              q16;
    uint16_t              tmp;
    uint16_t              w16;
    uint16_t              h16;
    uint16_t              persist16 = 1U;
    bool_t                persist;
    char                  resp[224];

    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"ok\":false,\"error\":\"bad_body\"}");
    }

    received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, "{\"ok\":false,\"error\":\"bad_body\"}");
    }
    body[received] = '\0';

    nvs_camera_settings_default(&st);
    st.web_width   = camera_sensor_get_web_width();
    st.web_height  = camera_sensor_get_web_height();
    st.quality     = camera_sensor_get_jpeg_quality();
    st.grayscale   = camera_sensor_get_grayscale();
    st.zoom        = camera_sensor_get_zoom();
    st.img_rotate  = camera_sensor_get_img_rotate();
    st.flip_v      = camera_sensor_get_flip_v();
    st.flip_h      = camera_sensor_get_flip_h();

    if (strstr(body, "240x240") != NULL) {
        st.web_width  = 240U;
        st.web_height = 240U;
    } else if (strstr(body, "320x240") != NULL) {
        /* 采集已为 240×240，更大尺寸无收益，落到原生。 */
        st.web_width  = 240U;
        st.web_height = 240U;
    } else if (strstr(body, "640x480") != NULL) {
        st.web_width  = 240U;
        st.web_height = 240U;
    } else {
        w16 = st.web_width;
        h16 = st.web_height;
        if (web_parse_u16_after_key(body, "\"width\"", &w16)) {
            st.web_width = w16;
        }
        if (web_parse_u16_after_key(body, "\"height\"", &h16)) {
            st.web_height = h16;
        }
    }

    q16 = st.quality;
    if (web_parse_u16_after_key(body, "\"quality\"", &q16)) {
        st.quality = (uint8_t)q16;
    }
    tmp = st.grayscale;
    if (web_parse_u16_after_key(body, "\"grayscale\"", &tmp)) {
        st.grayscale = (uint8_t)tmp;
    }
    tmp = st.zoom;
    if (web_parse_u16_after_key(body, "\"zoom\"", &tmp)) {
        st.zoom = (uint8_t)tmp;
    }
    tmp = st.img_rotate;
    if (web_parse_u16_after_key(body, "\"rotate\"", &tmp)) {
        st.img_rotate = (uint8_t)tmp;
    }
    tmp = st.flip_v;
    if (web_parse_u16_after_key(body, "\"flip_v\"", &tmp)) {
        st.flip_v = (uint8_t)tmp;
    }
    tmp = st.flip_h;
    if (web_parse_u16_after_key(body, "\"flip_h\"", &tmp)) {
        st.flip_h = (uint8_t)tmp;
    }
    if (web_parse_u16_after_key(body, "\"persist\"", &persist16)) {
        /* keep parsed value */
    } else {
        persist16 = 1U; /* 未带 persist 字段时一律落盘 */
    }
    /* 安全默认：除非明确 persist:0，否则写入 NVS */
    persist = (persist16 != 0U) ? TRUE : FALSE;

    if (camera_sensor_apply_settings(&st, persist) != STATUS_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req,
                             (persist != FALSE) ? "{\"ok\":false,\"error\":\"save_failed\"}"
                                                : "{\"ok\":false,\"error\":\"bad_cfg\"}");
    }

    (void)snprintf(resp,
                   sizeof(resp),
                   "{\"ok\":true,\"saved\":%s,\"cfg_width\":%u,\"cfg_height\":%u,"
                   "\"width\":%u,\"height\":%u,\"quality\":%u,\"poll_ms\":%u,"
                   "\"grayscale\":%u,\"zoom\":%u,\"rotate\":%u,\"flip_v\":%u,\"flip_h\":%u}",
                   (persist != FALSE) ? "true" : "false",
                   (unsigned)st.web_width,
                   (unsigned)st.web_height,
                   (unsigned)camera_sensor_get_snapshot_width(),
                   (unsigned)camera_sensor_get_snapshot_height(),
                   (unsigned)st.quality,
                   (unsigned)camera_sensor_get_web_poll_ms(),
                   (unsigned)st.grayscale,
                   (unsigned)st.zoom,
                   (unsigned)st.img_rotate,
                   (unsigned)st.flip_v,
                   (unsigned)st.flip_h);
    return web_send_json(req, resp);
}

esp_err_t web_pages_register(httpd_handle_t server)
{
    httpd_uri_t status = {
        .uri     = "/api/camera/status",
        .method  = HTTP_GET,
        .handler = web_api_status_get,
    };
    httpd_uri_t preview_page = {
        .uri     = "/preview.html",
        .method  = HTTP_GET,
        .handler = web_pages_preview_get_handler,
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
    httpd_uri_t camera_mjpeg = {
        .uri     = "/api/camera/stream.mjpg",
        .method  = HTTP_GET,
        .handler = web_api_camera_mjpeg_get,
    };
    httpd_uri_t camera_resume = {
        .uri     = "/api/camera/camera_resume",
        .method  = HTTP_POST,
        .handler = web_api_camera_resume_post,
    };
    httpd_uri_t camera_pause = {
        .uri     = "/api/camera/camera_pause",
        .method  = HTTP_POST,
        .handler = web_api_camera_pause_post,
    };
    httpd_uri_t camera_cfg = {
        .uri     = "/api/camera/config",
        .method  = HTTP_POST,
        .handler = web_api_camera_config_post,
    };
    httpd_uri_t servo_status = {
        .uri     = "/api/servo/status",
        .method  = HTTP_GET,
        .handler = web_api_servo_status_get,
    };
    httpd_uri_t servo_post = {
        .uri     = "/api/servo",
        .method  = HTTP_POST,
        .handler = web_api_servo_post,
    };
    httpd_uri_t app_mode = {
        .uri     = "/api/app_mode",
        .method  = HTTP_GET,
        .handler = web_api_app_mode_get,
    };
    httpd_uri_t servo_calib_get = {
        .uri     = "/api/servo/calib",
        .method  = HTTP_GET,
        .handler = web_api_servo_calib_get,
    };
    httpd_uri_t servo_calib_post = {
        .uri     = "/api/servo/calib",
        .method  = HTTP_POST,
        .handler = web_api_servo_calib_post,
    };
    esp_err_t err;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    web_ctrl_wifi_set_prepare_hook(web_pages_wifi_prepare_hook);
    web_ctrl_wifi_set_pause_hook(web_pages_wifi_pause_hook);
#if !CAMERA_APP_CALIB_MODE
    (void)web_jpeg_buf_ensure();
#endif
    web_servo_calib_draft_ensure();
#if CAMERA_APP_CALIB_MODE
    s_web_camera_paused = true;
    ESP_LOGI(TAG, "calib mode: MJPEG disabled, stream paused by default");
#endif

    err = web_pages_register_uri(server, &favicon);
    if (err != ESP_OK) {
        return err;
    }
#if !CAMERA_APP_CALIB_MODE
    err = web_pages_register_uri(server, &preview_page);
    if (err != ESP_OK) {
        return err;
    }
#endif
    err = web_pages_register_uri(server, &status);
    if (err != ESP_OK) {
        return err;
    }
#if !CAMERA_APP_CALIB_MODE
    err = web_pages_register_uri(server, &camera_jpeg);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &camera_mjpeg);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &camera_resume);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &camera_pause);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &camera_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &servo_status);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &servo_post);
    if (err != ESP_OK) {
        return err;
    }
#else
    (void)preview_page;
    (void)camera_jpeg;
    (void)camera_mjpeg;
    (void)camera_resume;
    (void)camera_pause;
    (void)camera_cfg;
    (void)servo_status;
    (void)servo_post;
#endif
    err = web_pages_register_uri(server, &app_mode);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &servo_calib_get);
    if (err != ESP_OK) {
        return err;
    }
    return web_pages_register_uri(server, &servo_calib_post);
}
