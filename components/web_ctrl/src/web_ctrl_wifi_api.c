/**
 * @file web_ctrl_wifi_api.c
 * @brief SoftAP 网页配网 REST 与后台扫描任务。
 */

#include "web_ctrl_wifi_api.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "lwip/sockets.h"

#include "battery.h"
#include "net_wifi.h"
#include "nvs.h"

static const char *TAG = "web_ctrl_wifi";

#define WEB_CTRL_WIFI_JSON_BODY_MAX (384U)
#define WEB_CTRL_WIFI_SCAN_MAX (32U)
#define WEB_CTRL_WIFI_SCAN_TASK_STACK (4096U)
#define WEB_CTRL_WIFI_SCAN_TASK_PRIO (4U)
#define WEB_CTRL_WIFI_JSON_OUT (4096U)

static SemaphoreHandle_t s_mtx;
static TaskHandle_t    s_scan_task;
static bool            s_infra_ready;
static volatile bool   s_scan_busy;
static uint32_t        s_scan_generation;
static wifi_ap_record_t s_ap_buf[WEB_CTRL_WIFI_SCAN_MAX];
static uint16_t        s_ap_count;
static web_ctrl_wifi_prepare_fn s_prepare_hook;
static web_ctrl_wifi_prepare_fn s_pause_hook;
static EventGroupHandle_t       s_scan_evt;
static esp_event_handler_instance_t s_scan_evt_inst;

#define WEB_CTRL_WIFI_SCAN_DONE_BIT BIT0

void web_ctrl_wifi_set_prepare_hook(web_ctrl_wifi_prepare_fn fn)
{
    s_prepare_hook = fn;
}

void web_ctrl_wifi_set_pause_hook(web_ctrl_wifi_prepare_fn fn)
{
    s_pause_hook = fn;
}

static void wifi_invoke_prepare_hook(void)
{
    if (s_prepare_hook != NULL) {
        s_prepare_hook();
    }
}

static void wifi_invoke_pause_hook(void)
{
    if (s_pause_hook != NULL) {
        s_pause_hook();
    } else if (s_prepare_hook != NULL) {
        /* 兼容旧应用：仅注册了 prepare 时扫描也先停流。 */
        s_prepare_hook();
    }
}

static void wifi_scan_done_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (s_scan_evt != NULL) {
        (void)xEventGroupSetBits(s_scan_evt, WEB_CTRL_WIFI_SCAN_DONE_BIT);
    }
}

/**
 * @brief 按 `req->content_len` 读满 POST body（循环 `httpd_req_recv`，避免单次未读全）。
 * @note 不依赖 `Content-Length` 头名字符串：部分客户端/代理下 `httpd_req_get_hdr_value_len` 可能为 0，
 *       但解析器已写入 `req->content_len`。
 */
static esp_err_t wifi_http_read_post_body(httpd_req_t *req, char *body, size_t body_cap, size_t *out_len)
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
            ESP_LOGW(TAG, "post recv err %d at %u/%u", rlen, (unsigned int)total, (unsigned int)body_len);
            return ESP_FAIL;
        }
        if (rlen == 0) {
            ESP_LOGW(TAG, "post recv closed at %u/%u", (unsigned int)total, (unsigned int)body_len);
            return ESP_FAIL;
        }
        total += (size_t)rlen;
    }
    body[body_len] = '\0';
    *out_len = body_len;
    return ESP_OK;
}

static void scan_worker(void *arg)
{
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        {
            wifi_scan_config_t sc = {0};
            esp_err_t          start_err;

            sc.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
            sc.show_hidden = false;
            /* 缩短主动扫描驻留，减轻 SoftAP 卡顿。 */
            sc.scan_time.active.min = 80;
            sc.scan_time.active.max = 120;

            if (s_scan_evt != NULL) {
                (void)xEventGroupClearBits(s_scan_evt, WEB_CTRL_WIFI_SCAN_DONE_BIT);
            }

            start_err = esp_wifi_scan_start(&sc, false);
            if (start_err == ESP_OK) {
                if (s_scan_evt != NULL) {
                    (void)xEventGroupWaitBits(s_scan_evt,
                                              WEB_CTRL_WIFI_SCAN_DONE_BIT,
                                              pdTRUE,
                                              pdFALSE,
                                              pdMS_TO_TICKS(8000));
                } else {
                    vTaskDelay(pdMS_TO_TICKS(3000));
                }
            } else {
                ESP_LOGW(TAG, "esp_wifi_scan_start: %s", esp_err_to_name(start_err));
            }
        }

        {
            uint16_t        num = WEB_CTRL_WIFI_SCAN_MAX;
            const esp_err_t er  = esp_wifi_scan_get_ap_records(&num, s_ap_buf);

            if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
                if (er == ESP_OK) {
                    /* 同 SSID 去重，保留更强 RSSI，缩小 JSON、减轻网页渲染。 */
                    uint16_t uniq = 0U;
                    uint16_t i;

                    for (i = 0U; i < num; i++) {
                        uint16_t j;
                        bool     merged = false;

                        if (s_ap_buf[i].ssid[0] == '\0') {
                            continue;
                        }
                        for (j = 0U; j < uniq; j++) {
                            if (strncmp((const char *)s_ap_buf[i].ssid,
                                        (const char *)s_ap_buf[j].ssid,
                                        sizeof(s_ap_buf[i].ssid)) == 0) {
                                if (s_ap_buf[i].rssi > s_ap_buf[j].rssi) {
                                    s_ap_buf[j] = s_ap_buf[i];
                                }
                                merged = true;
                                break;
                            }
                        }
                        if (!merged) {
                            if (uniq != i) {
                                s_ap_buf[uniq] = s_ap_buf[i];
                            }
                            uniq++;
                        }
                    }
                    s_ap_count = uniq;
                } else {
                    s_ap_count = 0U;
                }
                s_scan_generation++;
                s_scan_busy = false;
                (void)xSemaphoreGive(s_mtx);
            }
        }
    }
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

static bool json_extract_quoted_field(const char *body, const char *key, char *out, size_t out_cap)
{
    const char *found = strstr(body, key);
    const char *colon;
    const char *p;
    size_t      o = 0U;

    if ((found == NULL) || (out_cap == 0U)) {
        return false;
    }
    colon = strchr(found + strlen(key), ':');
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
        out[o++] = *p++;
    }
    if (*p != '"') {
        return false;
    }
    out[o] = '\0';
    return true;
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

static const char *net_mode_to_str(net_wifi_mode_t m)
{
    switch (m) {
    case NET_WIFI_MODE_SOFTAP:
        return "softap";
    case NET_WIFI_MODE_STA:
        return "sta";
    default:
        return "off";
    }
}

static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    char                 json[512];
    char                 ipbuf[20];
    const net_wifi_mode_t m = net_wifi_get_mode();
    int                   n;
    battery_info_t        bi;
    battery_voltage_t     bv;
    bool_t                bat_ok;

    (void)net_wifi_format_ipv4_for_display(ipbuf, sizeof(ipbuf));

    bat_ok = battery_percent_update();
    if ((bat_ok != FALSE) && (battery_info_read(&bi, &bv) != FALSE)) {
        n = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"mode\":\"%s\",\"wifi_started\":%s,\"sta_has_ip\":%s,\"softap\":%s,\"ip\":\"%s\","
                     "\"battery\":{\"valid\":true,\"percent\":%u,\"mv\":%u,\"charging\":%s,\"level\":%u}}",
                     net_mode_to_str(m), net_wifi_is_started() ? "true" : "false",
                     net_wifi_sta_has_ipv4() ? "true" : "false", net_wifi_is_softap_mode() ? "true" : "false", ipbuf,
                     (unsigned int)bi.percent, (unsigned int)bv.current_mv, (bi.charging != FALSE) ? "true" : "false",
                     (unsigned int)bi.level);
    } else {
        n = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"mode\":\"%s\",\"wifi_started\":%s,\"sta_has_ip\":%s,\"softap\":%s,\"ip\":\"%s\","
                     "\"battery\":{\"valid\":false}}",
                     net_mode_to_str(m), net_wifi_is_started() ? "true" : "false",
                     net_wifi_sta_has_ipv4() ? "true" : "false", net_wifi_is_softap_mode() ? "true" : "false", ipbuf);
    }
    if ((n <= 0) || ((size_t)n >= sizeof(json))) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (size_t)n);
}

static esp_err_t wifi_scan_post_handler(httpd_req_t *req)
{
    (void)req;

    if (!net_wifi_is_started()) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"wifi_down\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* 先停 MJPEG 等长连接，释放 SoftAP 下有限的 httpd 槽位。 */
    wifi_invoke_pause_hook();

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"lock\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (s_scan_busy) {
        (void)xSemaphoreGive(s_mtx);
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"scan_busy\"}", HTTPD_RESP_USE_STRLEN);
    }
    s_scan_busy = true;
    (void)xSemaphoreGive(s_mtx);

    if (s_scan_task != NULL) {
        (void)xTaskNotifyGive(s_scan_task);
    }

    (void)httpd_resp_set_status(req, "202 Accepted");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_scan_result_get_handler(httpd_req_t *req)
{
    char json[WEB_CTRL_WIFI_JSON_OUT];
    size_t j = 0U;

    (void)req;

    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"lock\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (s_scan_busy) {
        (void)xSemaphoreGive(s_mtx);
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"pending\":true}", HTTPD_RESP_USE_STRLEN);
    }

    j = (size_t)snprintf(json, sizeof(json), "{\"ok\":true,\"gen\":%" PRIu32 ",\"aps\":[", s_scan_generation);
    if (j >= sizeof(json)) {
        (void)xSemaphoreGive(s_mtx);
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"overflow\"}", HTTPD_RESP_USE_STRLEN);
    }

    for (uint16_t i = 0U; i < s_ap_count; i++) {
        const wifi_ap_record_t *r = &s_ap_buf[i];
        char                    ess[70];
        char                    esc[140];
        int                     w;

        if (r->ssid[0] == '\0') {
            (void)snprintf(ess, sizeof(ess), "(hidden)");
        } else {
            const size_t sl = strnlen((const char *)r->ssid, sizeof(r->ssid));

            (void)snprintf(ess, sizeof(ess), "%.*s", (int)sl, (const char *)r->ssid);
        }
        (void)json_escape_to_buf(ess, esc, sizeof(esc));
        w = snprintf(json + j, sizeof(json) - j, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u,\"auth\":%u}",
                     (i > 0U) ? "," : "", esc, (int)r->rssi, (unsigned int)r->primary, (unsigned int)r->authmode);
        if ((w <= 0) || ((size_t)w >= sizeof(json) - j)) {
            break;
        }
        j += (size_t)w;
    }
    if (j + 4U < sizeof(json)) {
        (void)memcpy(json + j, "]}\0", 4U);
    } else {
        json[sizeof(json) - 1U] = '\0';
    }

    (void)xSemaphoreGive(s_mtx);
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static bool client_on_softap_lan(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage peer;
    socklen_t               slen = (socklen_t)sizeof(peer);

    if (fd < 0) {
        return false;
    }
    if (getpeername(fd, (struct sockaddr *)&peer, &slen) != 0) {
        return false;
    }
    if (peer.ss_family == AF_INET) {
        const struct sockaddr_in *in4 = (const struct sockaddr_in *)&peer;

        return net_wifi_softap_peer_ipv4_on_ap_subnet(in4->sin_addr.s_addr);
    }
#if CONFIG_LWIP_IPV6
    if (peer.ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&peer;
        const uint8_t               *b = in6->sin6_addr.s6_addr;

        /* fe80::/10：部分终端用 http://[fe80::…]/ 访问 AP，对端为 IPv6 链路本地地址。 */
        if ((b[0] == 0xfeU) && ((b[1] & 0xc0U) == 0x80U)) {
            return true;
        }
    }
#endif
    return false;
}

/**
 * @brief 浏览器以 `http://<AP_IP>/` 打开时，`Host` 常为 AP 地址；与 `getpeername` 子网判断互为补充（避免误 403）。
 */
static bool wifi_save_softap_host_matches_ap(httpd_req_t *req)
{
    char      ap_ip[20];
    char      host[80];
    esp_err_t herr;
    char     *colon;
    size_t    hlen;

    if (!net_wifi_format_ipv4_for_display(ap_ip, sizeof(ap_ip))) {
        return false;
    }
    herr = httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if (herr != ESP_OK) {
        return false;
    }
    colon = strchr(host, ':');
    if (colon != NULL) {
        *colon = '\0';
    }
    hlen = strlen(host);
    while ((hlen > 0U) && ((host[hlen - 1U] == ' ') || (host[hlen - 1U] == '\t'))) {
        host[--hlen] = '\0';
    }
    return (strcmp(host, ap_ip) == 0);
}

static bool wifi_save_request_allowed(httpd_req_t *req)
{
    if (!net_wifi_is_softap_mode()) {
        return true;
    }
    if (client_on_softap_lan(req)) {
        return true;
    }
    if (wifi_save_softap_host_matches_ap(req)) {
        return true;
    }
    /* 部分手机/PC 在 SoftAP 下 getpeername/Host 形态不一致；有 POST body 时仍允许配网写入。 */
    return (req != NULL) && (req->content_len > 0U);
}

static void wifi_delayed_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void wifi_schedule_reboot(void)
{
    if (xTaskCreate(wifi_delayed_reboot_task, "wifi_rb", 2048, NULL, 5, NULL) != pdPASS) {
        esp_restart();
    }
}

static esp_err_t wifi_save_post_handler(httpd_req_t *req)
{
    char                    body[WEB_CTRL_WIFI_JSON_BODY_MAX + 1U];
    size_t                  body_len = 0U;
    esp_err_t               rbody;
    char                    ssid[33];
    char                    password[65];
    nvs_web_ctrl_settings_t st;

    if (!wifi_save_request_allowed(req)) {
        (void)httpd_resp_set_status(req, "403 Forbidden");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"forbidden\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* 尽早停流，避免读 body / 写 NVS 时 MJPEG 仍占槽。 */
    wifi_invoke_pause_hook();

    rbody = wifi_http_read_post_body(req, body, sizeof(body), &body_len);
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

    if (!json_extract_quoted_field(body, "\"ssid\"", ssid, sizeof(ssid))) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need ssid string\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (strstr(body, "\"password\"") != NULL) {
        if (!json_extract_quoted_field(body, "\"password\"", password, sizeof(password))) {
            (void)httpd_resp_set_status(req, "400 Bad Request");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad password field\"}", HTTPD_RESP_USE_STRLEN);
        }
    } else {
        password[0] = '\0';
    }

    if (strnlen(ssid, sizeof(ssid)) == 0U) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"empty ssid\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!nvs_web_ctrl_settings_get(&st)) {
        nvs_web_ctrl_settings_default(&st);
    }
    (void)strncpy(st.sta_ssid, ssid, sizeof(st.sta_ssid) - 1U);
    st.sta_ssid[sizeof(st.sta_ssid) - 1U] = '\0';
    (void)strncpy(st.sta_password, password, sizeof(st.sta_password) - 1U);
    st.sta_password[sizeof(st.sta_password) - 1U] = '\0';
    st.magic                                     = NVS_WEB_CTRL_MAGIC;

    if (!nvs_web_ctrl_settings_validate(&st)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!nvs_web_ctrl_settings_set(&st)) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"nvs\"}", HTTPD_RESP_USE_STRLEN);
    }

    wifi_invoke_prepare_hook();

    ESP_LOGI(TAG, "STA saved ssid=\"%s\", reboot scheduled", ssid);
    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_send(req, "{\"ok\":true,\"reboot\":true}", HTTPD_RESP_USE_STRLEN);
    wifi_schedule_reboot();
    return ESP_OK;
}

static esp_err_t wifi_sta_disconnect_post_handler(httpd_req_t *req)
{
    esp_err_t werr;

    if (!wifi_save_request_allowed(req)) {
        (void)httpd_resp_set_status(req, "403 Forbidden");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"forbidden\"}", HTTPD_RESP_USE_STRLEN);
    }

    werr = net_wifi_sta_disconnect();
    (void)httpd_resp_set_type(req, "application/json");
    if (werr != ESP_OK) {
        char json[96];
        int  n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(werr));

        if ((n <= 0) || ((size_t)n >= sizeof(json))) {
            (void)httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"disconnect\"}", HTTPD_RESP_USE_STRLEN);
        }
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    }
    if (!nvs_web_ctrl_settings_clear_sta_credentials()) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"nvs_clear_sta\"}", HTTPD_RESP_USE_STRLEN);
    }

    wifi_invoke_prepare_hook();

    (void)httpd_resp_send(req, "{\"ok\":true,\"reboot\":true}", HTTPD_RESP_USE_STRLEN);
    wifi_schedule_reboot();
    return ESP_OK;
}

static esp_err_t wifi_sta_try_post_handler(httpd_req_t *req)
{
    (void)req;
    (void)httpd_resp_set_status(req, "501 Not Implemented");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"use POST /api/wifi/save\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t provision_get_handler(httpd_req_t *req)
{
    extern const char provision_html_start[] asm("_binary_provision_html_start");
    extern const char provision_html_end[] asm("_binary_provision_html_end");
    const size_t len = (size_t)(provision_html_end - provision_html_start);

    (void)httpd_resp_set_type(req, "text/html; charset=utf-8");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, provision_html_start, len);
}

esp_err_t web_ctrl_wifi_api_register(httpd_handle_t server)
{
    esp_err_t err;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_infra_ready) {
        if (s_mtx == NULL) {
            s_mtx = xSemaphoreCreateMutex();
            if (s_mtx == NULL) {
                ESP_LOGE(TAG, "mutex create failed");
                return ESP_ERR_NO_MEM;
            }
        }

        if (s_scan_evt == NULL) {
            s_scan_evt = xEventGroupCreate();
            if (s_scan_evt == NULL) {
                ESP_LOGE(TAG, "scan event group create failed");
                return ESP_ERR_NO_MEM;
            }
        }

        if (s_scan_evt_inst == NULL) {
            const esp_err_t e =
                esp_event_handler_instance_register(WIFI_EVENT,
                                                    WIFI_EVENT_SCAN_DONE,
                                                    wifi_scan_done_event,
                                                    NULL,
                                                    &s_scan_evt_inst);
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "SCAN_DONE register: %s (fallback delay)", esp_err_to_name(e));
            }
        }

        if (s_scan_task == NULL) {
            const BaseType_t ok =
                xTaskCreate(scan_worker, "w_wifi_scan", WEB_CTRL_WIFI_SCAN_TASK_STACK, NULL,
                            WEB_CTRL_WIFI_SCAN_TASK_PRIO, &s_scan_task);
            if (ok != pdPASS) {
                ESP_LOGE(TAG, "scan task create failed");
                return ESP_ERR_NO_MEM;
            }
        }
        s_infra_ready = true;
    }

    {
        const httpd_uri_t u_status = {.uri      = "/api/wifi/status",
                                      .method   = HTTP_GET,
                                      .handler  = wifi_status_get_handler,
                                      .user_ctx = NULL};
        const httpd_uri_t u_scan = {.uri      = "/api/wifi/scan",
                                    .method   = HTTP_POST,
                                    .handler  = wifi_scan_post_handler,
                                    .user_ctx = NULL};
        const httpd_uri_t u_scanr = {.uri      = "/api/wifi/scan_result",
                                     .method   = HTTP_GET,
                                     .handler  = wifi_scan_result_get_handler,
                                     .user_ctx = NULL};
        const httpd_uri_t u_save = {.uri      = "/api/wifi/save",
                                    .method   = HTTP_POST,
                                    .handler  = wifi_save_post_handler,
                                    .user_ctx = NULL};
        const httpd_uri_t u_disc = {.uri      = "/api/wifi/sta_disconnect",
                                    .method   = HTTP_POST,
                                    .handler  = wifi_sta_disconnect_post_handler,
                                    .user_ctx = NULL};
        const httpd_uri_t u_try = {.uri = "/api/wifi/sta", .method = HTTP_POST, .handler = wifi_sta_try_post_handler,
                                   .user_ctx = NULL};
        const httpd_uri_t u_prov = {.uri = "/provision", .method = HTTP_GET, .handler = provision_get_handler,
                                    .user_ctx = NULL};

        err = httpd_register_uri_handler(server, &u_status);
        if (err != ESP_OK) {
            goto fail;
        }
        err = httpd_register_uri_handler(server, &u_scan);
        if (err != ESP_OK) {
            goto fail;
        }
        err = httpd_register_uri_handler(server, &u_scanr);
        if (err != ESP_OK) {
            goto fail;
        }
        err = httpd_register_uri_handler(server, &u_save);
        if (err != ESP_OK) {
            goto fail;
        }
        err = httpd_register_uri_handler(server, &u_disc);
        if (err != ESP_OK) {
            goto fail;
        }
        err = httpd_register_uri_handler(server, &u_try);
        if (err != ESP_OK) {
            goto fail;
        }
        err = httpd_register_uri_handler(server, &u_prov);
        if (err != ESP_OK) {
            goto fail;
        }
    }

    ESP_LOGI(TAG, "wifi provisioning URIs registered");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "register uri failed: %s", esp_err_to_name(err));
    return err;
}
