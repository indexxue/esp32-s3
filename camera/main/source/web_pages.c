/**
 * @file web_pages.c
 * @brief camera HTTP 页面与 REST：根页面、状态、JPEG 快照、MJPEG 流。
 */

#include "web_pages.h"

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
#include "net_wifi.h"
#include "web_ctrl_wifi_api.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static const char *TAG = "web_pages";

/** 640×480 JPEG 余量；预览优先走 sensor 侧 cache。 */
#define CAMERA_WEB_JPEG_BUF_CAP (98304U)
#define CAMERA_WEB_MJPEG_BOUNDARY "frame"
#define CAMERA_WEB_MJPEG_MAX_FRAMES (0U) /* 0 = 直到客户端断开 */

static volatile bool s_web_camera_paused;
static uint8_t            *s_web_jpeg_buf;
static SemaphoreHandle_t   s_web_jpeg_mtx;
static volatile bool       s_mjpeg_busy;

static void web_pages_wifi_prepare_hook(void)
{
    s_web_camera_paused = true;
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
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    (void)httpd_resp_set_hdr(req, "Pragma", "no-cache");
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
    char body[480];

    (void)snprintf(body,
                   sizeof(body),
                   "{\"product\":\"camera\",\"camera\":%s,\"width\":%u,\"height\":%u,"
                   "\"quality\":%u,\"poll_ms\":%u,\"grayscale\":%u,\"zoom\":%u,"
                   "\"rotate\":%u,\"flip_v\":%u,\"flip_h\":%u,"
                   "\"cfg_width\":%u,\"cfg_height\":%u,"
                   "\"frames\":%u,\"lcd_blit\":%u,\"paused\":%s,"
                   "\"stream\":\"/api/camera/stream.mjpg\","
                   "\"sizes\":[\"240x240\",\"320x240\",\"640x480\"]}",
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
                   s_web_camera_paused ? "true" : "false");
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

    if (!web_local_peer_allowed(req)) {
        return web_sensitive_forbidden(req);
    }
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

        if (camera_sensor_wait_jpeg_seq(&seq, 1000U) != STATUS_OK) {
            /* 无新帧：发空闲探测，避免中间代理/浏览器以为挂死。 */
            continue;
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
    ESP_LOGI(TAG, "mjpeg end frames=%u err=%s", (unsigned)frames, esp_err_to_name(err));
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
        st.web_width  = 320U;
        st.web_height = 240U;
    } else if (strstr(body, "640x480") != NULL) {
        st.web_width  = 640U;
        st.web_height = 480U;
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
    esp_err_t err;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    web_ctrl_wifi_set_prepare_hook(web_pages_wifi_prepare_hook);
    (void)web_jpeg_buf_ensure();

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
    err = web_pages_register_uri(server, &camera_mjpeg);
    if (err != ESP_OK) {
        return err;
    }
    err = web_pages_register_uri(server, &camera_resume);
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
    return web_pages_register_uri(server, &servo_post);
}
