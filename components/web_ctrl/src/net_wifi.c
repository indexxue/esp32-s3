/**
 * @file net_wifi.c
 * @brief Wi‑Fi SoftAP / STA 启动与事件日志。
 */

#include "net_wifi.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "esp_wifi_default.h"

static const char *TAG = "net_wifi";

#ifndef CONFIG_NET_WIFI_STA_AP_FALLBACK_TASK_STACK
#define CONFIG_NET_WIFI_STA_AP_FALLBACK_TASK_STACK (4096U)
#endif
#ifndef CONFIG_NET_WIFI_STA_AP_FALLBACK_TASK_PRIO
#define CONFIG_NET_WIFI_STA_AP_FALLBACK_TASK_PRIO (5U)
#endif

/** WPA2-PSK 最小密码长度（字节）。 */
static const unsigned int k_softap_wpa2_min_pass_len = 8U;

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler_inst;
static esp_event_handler_instance_t s_ip_handler_inst;
static net_wifi_ipv4_event_fn       s_ipv4_event_fn;
static bool s_netif_stack_inited;
static bool s_wifi_driver_inited;
static bool s_wifi_iface_started;
static net_wifi_mode_t s_running_mode = NET_WIFI_MODE_OFF;

/** `IP_EVENT_STA_GOT_IP` 同步位。 */
static const EventBits_t k_sta_got_ip_bit = (EventBits_t)(1U << 0);

void net_wifi_set_ipv4_event_handler(net_wifi_ipv4_event_fn fn)
{
    s_ipv4_event_fn = fn;
}

static void net_wifi_notify_ipv4_event(void)
{
    if (s_ipv4_event_fn != NULL) {
        s_ipv4_event_fn();
    }
}
static EventGroupHandle_t s_sta_ip_event_group;

static void destroy_ap_netif(void)
{
    if (s_ap_netif == NULL) {
        return;
    }
    (void)esp_wifi_clear_default_wifi_driver_and_handlers(s_ap_netif);
    esp_netif_destroy(s_ap_netif);
    s_ap_netif = NULL;
}

static void destroy_sta_netif(void)
{
    if (s_sta_netif == NULL) {
        return;
    }
    (void)esp_wifi_clear_default_wifi_driver_and_handlers(s_sta_netif);
    esp_netif_destroy(s_sta_netif);
    s_sta_netif = NULL;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started");
        if (s_ap_netif != NULL) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
                ESP_LOGI(TAG, "SoftAP IP: " IPSTR, IP2STR(&ip.ip));
            }
        }
    } else if (event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, "SoftAP stopped");
    } else if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting...");
        if (s_sta_ip_event_group != NULL) {
            (void)xEventGroupClearBits(s_sta_ip_event_group, k_sta_got_ip_bit);
        }
        net_wifi_notify_ipv4_event();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA connected to AP");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta_ip_event_group != NULL) {
            (void)xEventGroupClearBits(s_sta_ip_event_group, k_sta_got_ip_bit);
        }
        ESP_LOGW(TAG, "STA disconnected");
        net_wifi_notify_ipv4_event();
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base != IP_EVENT) {
        return;
    }

    if (event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        if (s_sta_ip_event_group != NULL) {
            (void)xEventGroupSetBits(s_sta_ip_event_group, k_sta_got_ip_bit);
        }
        net_wifi_notify_ipv4_event();
    }
}

static esp_err_t ensure_event_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t validate_softap_config(const net_wifi_config_t *cfg)
{
    const size_t ssid_len = strnlen(cfg->softap_ssid, sizeof(cfg->softap_ssid));
    if (ssid_len == 0U || ssid_len > 32U) {
        ESP_LOGE(TAG, "SoftAP SSID length invalid: %u", (unsigned int)ssid_len);
        return ESP_ERR_INVALID_ARG;
    }

    const size_t pass_len = strnlen(cfg->softap_password, sizeof(cfg->softap_password));
    if (pass_len > 0U && pass_len < k_softap_wpa2_min_pass_len) {
        ESP_LOGE(TAG, "SoftAP password must be empty (open) or >= %u chars", k_softap_wpa2_min_pass_len);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t validate_sta_config(const net_wifi_config_t *cfg)
{
    const size_t ssid_len = strnlen(cfg->sta_ssid, sizeof(cfg->sta_ssid));
    if (ssid_len == 0U || ssid_len > 32U) {
        ESP_LOGE(TAG, "STA SSID empty or too long");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t wifi_register_events(void)
{
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
                                                        &s_wifi_handler_inst);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL,
                                              &s_ip_handler_inst);
    return err;
}

static void wifi_unregister_events(void)
{
    if (s_wifi_handler_inst != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler_inst);
        s_wifi_handler_inst = NULL;
    }
    if (s_ip_handler_inst != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler_inst);
        s_ip_handler_inst = NULL;
    }
}

static esp_err_t wifi_driver_init_once(void)
{
    if (s_wifi_driver_inited) {
        return ESP_OK;
    }

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_driver_inited = true;
    return ESP_OK;
}

static void rollback_softap_setup(void)
{
    destroy_ap_netif();
    destroy_sta_netif();
    if (s_wifi_driver_inited) {
        (void)esp_wifi_deinit();
        s_wifi_driver_inited = false;
    }
}

static esp_err_t start_softap(const net_wifi_config_t *cfg)
{
    esp_err_t err = validate_softap_config(cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
        return ESP_FAIL;
    }

    /* SoftAP 网页扫描走 STA 侧：`WIFI_MODE_AP` 下扫描易异常；APSTA + 未连接 STA 为 IDF 常见用法。 */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        destroy_ap_netif();
        return ESP_FAIL;
    }

    err = wifi_driver_init_once();
    if (err != ESP_OK) {
        destroy_sta_netif();
        destroy_ap_netif();
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode APSTA failed: %s", esp_err_to_name(err));
        rollback_softap_setup();
        return err;
    }

    wifi_config_t wifi_config = {0};
    (void)snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%.31s", cfg->softap_ssid);
    wifi_config.ap.ssid_len = (uint8_t)strnlen((const char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid));

    const size_t pass_len = strnlen(cfg->softap_password, sizeof(cfg->softap_password));
    if (pass_len >= k_softap_wpa2_min_pass_len) {
        (void)snprintf((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), "%.63s", cfg->softap_password);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        wifi_config.ap.password[0] = '\0';
    }

    uint8_t ch = cfg->softap_channel;
    if (ch == 0U) {
        ch = 1U;
    }
    wifi_config.ap.channel = ch;

    uint8_t max_conn = cfg->softap_max_connection;
    if (max_conn == 0U) {
        max_conn = 4U;
    }
    if (max_conn > 10U) {
        max_conn = 10U;
    }
    wifi_config.ap.max_connection = max_conn;

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config AP failed: %s", esp_err_to_name(err));
        rollback_softap_setup();
        return err;
    }

    {
        wifi_config_t sta_wifi = {0};

        err = esp_wifi_set_config(WIFI_IF_STA, &sta_wifi);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config STA (idle) failed: %s", esp_err_to_name(err));
            rollback_softap_setup();
            return err;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start AP failed: %s", esp_err_to_name(err));
        rollback_softap_setup();
        return err;
    }

    /* 关闭 modem 省电：默认 MIN_MODEM 会造成 HTTP 卡顿与丢包（网页预览不可用）。 */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    s_running_mode = NET_WIFI_MODE_SOFTAP;
    ESP_LOGI(TAG, "SoftAP SSID=%s channel=%u", cfg->softap_ssid, (unsigned int)ch);
    return ESP_OK;
}

static void rollback_sta_setup(void)
{
    destroy_sta_netif();
    if (s_sta_ip_event_group != NULL) {
        vEventGroupDelete(s_sta_ip_event_group);
        s_sta_ip_event_group = NULL;
    }
    if (s_wifi_driver_inited) {
        (void)esp_wifi_deinit();
        s_wifi_driver_inited = false;
    }
}

static esp_err_t start_sta(const net_wifi_config_t *cfg)
{
    esp_err_t err = validate_sta_config(cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (s_sta_ip_event_group == NULL) {
        s_sta_ip_event_group = xEventGroupCreate();
        if (s_sta_ip_event_group == NULL) {
            ESP_LOGE(TAG, "xEventGroupCreate failed");
            return ESP_ERR_NO_MEM;
        }
    }
    (void)xEventGroupClearBits(s_sta_ip_event_group, k_sta_got_ip_bit);

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    err = wifi_driver_init_once();
    if (err != ESP_OK) {
        destroy_sta_netif();
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode STA failed: %s", esp_err_to_name(err));
        rollback_sta_setup();
        return err;
    }

    wifi_config_t wifi_config = {0};
    (void)snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%.31s", cfg->sta_ssid);
    (void)snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%.63s", cfg->sta_password);
    {
        const size_t sta_pass_len = strnlen(cfg->sta_password, sizeof(cfg->sta_password));
        if (sta_pass_len > 0U) {
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config STA failed: %s", esp_err_to_name(err));
        rollback_sta_setup();
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start STA failed: %s", esp_err_to_name(err));
        rollback_sta_setup();
        return err;
    }

    /* 关闭 modem 省电：默认 MIN_MODEM 会造成 HTTP 卡顿与丢包（网页预览不可用）。 */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        rollback_sta_setup();
        return err;
    }

    s_running_mode = NET_WIFI_MODE_STA;
    ESP_LOGI(TAG, "STA connecting to SSID=%s", cfg->sta_ssid);
    return ESP_OK;
}

void net_wifi_config_init_defaults(net_wifi_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    (void)memset(cfg, 0, sizeof(*cfg));
    cfg->mode = NET_WIFI_MODE_SOFTAP;
    (void)strncpy(cfg->softap_ssid, "ESP32-WebCtrl", sizeof(cfg->softap_ssid) - 1U);
    cfg->softap_ssid[sizeof(cfg->softap_ssid) - 1U] = '\0';
    (void)strncpy(cfg->softap_password, "esp32web1", sizeof(cfg->softap_password) - 1U);
    cfg->softap_password[sizeof(cfg->softap_password) - 1U] = '\0';
    cfg->softap_channel = 1U;
    cfg->softap_max_connection = 4U;
}

esp_err_t net_wifi_start(const net_wifi_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wifi_iface_started) {
        ESP_LOGW(TAG, "Wi‑Fi already started");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_netif_stack_inited) {
        const esp_err_t e = esp_netif_init();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(e));
            return e;
        }
        s_netif_stack_inited = true;
    }

    esp_err_t err = ensure_event_loop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_register_events();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register wifi events failed: %s", esp_err_to_name(err));
        return err;
    }

    if (cfg->mode == NET_WIFI_MODE_SOFTAP) {
        err = start_softap(cfg);
    } else if (cfg->mode == NET_WIFI_MODE_STA) {
        err = start_sta(cfg);
    } else {
        ESP_LOGE(TAG, "unsupported net_wifi_mode_t %d", (int)cfg->mode);
        err = ESP_ERR_NOT_SUPPORTED;
    }

    if (err != ESP_OK) {
        wifi_unregister_events();
        return err;
    }

    s_wifi_iface_started = true;
    return ESP_OK;
}

net_wifi_mode_t net_wifi_get_mode(void)
{
    if (!s_wifi_iface_started) {
        return NET_WIFI_MODE_OFF;
    }
    return s_running_mode;
}

bool net_wifi_sta_has_ipv4(void)
{
    if (!s_wifi_iface_started) {
        return false;
    }
    if (s_sta_netif != NULL) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0U) {
            return true;
        }
    }
    if (s_sta_ip_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_sta_ip_event_group) & k_sta_got_ip_bit) != 0U;
}

static bool read_netif_ipv4(esp_netif_t *netif, char *buf, size_t cap)
{
    esp_netif_ip_info_t ip;

    if ((netif == NULL) || (buf == NULL) || (cap < 8U)) {
        return false;
    }
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0U) {
        return false;
    }
    (void)snprintf(buf, cap, IPSTR, IP2STR(&ip.ip));
    return true;
}

bool net_wifi_format_ipv4_for_display(char *buf, size_t cap)
{
    if ((buf == NULL) || (cap < 4U)) {
        return false;
    }
    buf[0] = '\0';
    if (!s_wifi_iface_started) {
        (void)strncpy(buf, "---", cap - 1U);
        buf[cap - 1U] = '\0';
        return false;
    }

    /* STA 已 DHCP 时优先显示路由器分配的地址（含 APSTA 形态）。 */
    if (read_netif_ipv4(s_sta_netif, buf, cap)) {
        return true;
    }

    if (s_running_mode == NET_WIFI_MODE_SOFTAP) {
        if (read_netif_ipv4(s_ap_netif, buf, cap)) {
            return true;
        }
    }

    (void)strncpy(buf, "---", cap - 1U);
    buf[cap - 1U] = '\0';
    return false;
}

bool net_wifi_is_softap_mode(void)
{
    return s_wifi_iface_started && (s_running_mode == NET_WIFI_MODE_SOFTAP);
}

bool net_wifi_http_sensitive_peer_allowed(int sock_fd)
{
    struct sockaddr_storage peer;
    socklen_t               slen;

    if (!net_wifi_is_softap_mode()) {
        return false;
    }
    if (sock_fd < 0) {
        return false;
    }
    slen = (socklen_t)sizeof(peer);
    if (getpeername(sock_fd, (struct sockaddr *)&peer, &slen) != 0) {
        return false;
    }
    if (peer.ss_family == AF_INET) {
        const struct sockaddr_in *in4 = (const struct sockaddr_in *)&peer;

        return net_wifi_softap_peer_ipv4_on_ap_subnet(in4->sin_addr.s_addr);
    }
#if CONFIG_LWIP_IPV6
    if (peer.ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&peer;
        const uint8_t             *b  = in6->sin6_addr.s6_addr;

        if ((b[0] == 0xfeU) && ((b[1] & 0xc0U) == 0x80U)) {
            return true;
        }
    }
#endif
    return false;
}

bool net_wifi_softap_peer_ipv4_on_ap_subnet(uint32_t addr_nbo)
{
    esp_netif_ip_info_t info;
    uint32_t            ap_h;
    uint32_t            nm_h;
    uint32_t            peer_h;

    if ((s_ap_netif == NULL) || (s_running_mode != NET_WIFI_MODE_SOFTAP)) {
        return false;
    }
    if (esp_netif_get_ip_info(s_ap_netif, &info) != ESP_OK) {
        return false;
    }
    ap_h   = ntohl(info.ip.addr);
    nm_h   = ntohl(info.netmask.addr);
    peer_h  = ntohl(addr_nbo);
    if (nm_h == 0U) {
        return false;
    }
    return ((peer_h & nm_h) == (ap_h & nm_h));
}

/** 对端 IPv4 是否与指定 netif 同一子网。 */
static bool net_wifi_peer_ipv4_on_netif_subnet(esp_netif_t *netif, uint32_t addr_nbo)
{
    esp_netif_ip_info_t info;
    uint32_t            if_h;
    uint32_t            nm_h;
    uint32_t            peer_h;

    if (netif == NULL) {
        return false;
    }
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0U) {
        return false;
    }
    if_h   = ntohl(info.ip.addr);
    nm_h   = ntohl(info.netmask.addr);
    peer_h = ntohl(addr_nbo);
    if (nm_h == 0U) {
        return false;
    }
    return ((peer_h & nm_h) == (if_h & nm_h));
}

/** RFC1918 私网 IPv4（STA 跨 VLAN/掩码边缘时仍可预览）。 */
static bool net_wifi_ipv4_is_private(uint32_t addr_nbo)
{
    const uint32_t h = ntohl(addr_nbo);

    if ((h & 0xff000000U) == 0x0a000000U) {
        return true; /* 10.0.0.0/8 */
    }
    if ((h & 0xfff00000U) == 0xac100000U) {
        return true; /* 172.16.0.0/12 */
    }
    if ((h & 0xffff0000U) == 0xc0a80000U) {
        return true; /* 192.168.0.0/16 */
    }
    return false;
}

static bool net_wifi_peer_ipv4_allowed(uint32_t addr_nbo)
{
    if (net_wifi_peer_ipv4_on_netif_subnet(s_ap_netif, addr_nbo)) {
        return true;
    }
    if (net_wifi_peer_ipv4_on_netif_subnet(s_sta_netif, addr_nbo)) {
        return true;
    }
    /* STA：同局域网私网即可（手机 VPN/访客网关掩码与板子不一致时，严格同子网会误杀）。 */
    if ((s_running_mode == NET_WIFI_MODE_STA) && net_wifi_ipv4_is_private(addr_nbo)) {
        return true;
    }
    return false;
}

bool net_wifi_http_peer_on_local_subnet(int sock_fd)
{
    struct sockaddr_storage peer;
    socklen_t               slen;

    if (!s_wifi_iface_started || (sock_fd < 0)) {
        return false;
    }
    slen = (socklen_t)sizeof(peer);
    if (getpeername(sock_fd, (struct sockaddr *)&peer, &slen) != 0) {
        ESP_LOGW(TAG, "peer allow: getpeername fd=%d failed", sock_fd);
        return false;
    }
    if (peer.ss_family == AF_INET) {
        const struct sockaddr_in *in4 = (const struct sockaddr_in *)&peer;

        return net_wifi_peer_ipv4_allowed(in4->sin_addr.s_addr);
    }
#if CONFIG_LWIP_IPV6
    if (peer.ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&peer;
        const uint8_t             *b  = in6->sin6_addr.s6_addr;

        /* fe80::/10 链路本地 */
        if ((b[0] == 0xfeU) && ((b[1] & 0xc0U) == 0x80U)) {
            return true;
        }
        /* ::ffff:a.b.c.d（双栈栈上常见，浏览器走 IPv6 套接字） */
        if ((b[0] == 0U) && (b[1] == 0U) && (b[2] == 0U) && (b[3] == 0U) && (b[4] == 0U) &&
            (b[5] == 0U) && (b[6] == 0U) && (b[7] == 0U) && (b[8] == 0U) && (b[9] == 0U) &&
            (b[10] == 0xffU) && (b[11] == 0xffU)) {
            uint32_t v4;

            (void)memcpy(&v4, &b[12], sizeof(v4));
            return net_wifi_peer_ipv4_allowed(v4);
        }
    }
#endif
    return false;
}

esp_err_t net_wifi_wait_sta_got_ip(uint32_t timeout_ms)
{
    if ((s_sta_ip_event_group == NULL) || (!s_wifi_iface_started) || (s_running_mode != NET_WIFI_MODE_STA)) {
        return ESP_ERR_INVALID_STATE;
    }

    {
        const TickType_t ticks =
            (timeout_ms == 0U) ? 0U : pdMS_TO_TICKS(timeout_ms);
        const EventBits_t bits =
            xEventGroupWaitBits(s_sta_ip_event_group, k_sta_got_ip_bit, pdFALSE, pdTRUE, ticks);

        if ((bits & k_sta_got_ip_bit) != 0U) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t net_wifi_sta_disconnect(void)
{
    esp_err_t err;

    if (!s_wifi_iface_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((s_running_mode != NET_WIFI_MODE_STA) && (s_running_mode != NET_WIFI_MODE_SOFTAP)) {
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t net_wifi_stop(void)
{
    if (!s_wifi_iface_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }

    wifi_unregister_events();

    destroy_ap_netif();
    destroy_sta_netif();

    if (s_wifi_driver_inited) {
        err = esp_wifi_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_deinit: %s", esp_err_to_name(err));
        }
        s_wifi_driver_inited = false;
    }

    if (s_sta_ip_event_group != NULL) {
        vEventGroupDelete(s_sta_ip_event_group);
        s_sta_ip_event_group = NULL;
    }
    s_running_mode     = NET_WIFI_MODE_OFF;
    s_wifi_iface_started = false;
    return ESP_OK;
}

bool net_wifi_is_started(void)
{
    return s_wifi_iface_started;
}
