/**
 * @file net_wifi.h
 * @brief Wi‑Fi 与 TCP/IP 基座：SoftAP / STA 启动与停止。
 *
 * @note 调用方须已执行 `nvs_flash_init()`（本仓库通常在 `nvs_init` / 板级初始化之后）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NET_WIFI_MODE_OFF = 0,
    NET_WIFI_MODE_SOFTAP,
    NET_WIFI_MODE_STA,
} net_wifi_mode_t;

typedef struct {
    net_wifi_mode_t mode;
    /** SoftAP SSID，以 '\\0' 结尾，最长 32 字符（不含结束符）。 */
    char softap_ssid[33];
    /** SoftAP 密码；WPA2-PSK 至少 8 字符；空串表示开放热点（仅建议用于开发）。 */
    char softap_password[65];
    /** 信道 1～13；0 表示使用默认值 1。 */
    uint8_t softap_channel;
    /** 最大连接数 1～10。 */
    uint8_t softap_max_connection;
    /** STA SSID；STA 模式下不可为空。 */
    char sta_ssid[33];
    /** STA 密码；开放网络可为空。 */
    char sta_password[65];
} net_wifi_config_t;

/**
 * @brief 将配置填为库默认 SoftAP 参数（SSID/密码/信道等）。
 */
void net_wifi_config_init_defaults(net_wifi_config_t *cfg);

/**
 * @brief 按配置启动 Wi‑Fi（含 `esp_netif` / 事件循环，幂等：已启动则返回 `ESP_ERR_INVALID_STATE`）。
 */
esp_err_t net_wifi_start(const net_wifi_config_t *cfg);

/**
 * @brief 停止 Wi‑Fi 射频与驱动（不反初始化 NVS；便于与上层 HTTP 停止顺序配合）。
 */
esp_err_t net_wifi_stop(void);

/**
 * @brief 断开 STA 与当前 AP 的关联（`esp_wifi_disconnect`）；SoftAP+APSTA 时保留热点。
 * @return `ESP_ERR_INVALID_STATE` 表示 Wi‑Fi 未启动或当前非 STA/APSTA 运行形态。
 */
esp_err_t net_wifi_sta_disconnect(void);

bool net_wifi_is_started(void);

/** 当前 `net_wifi_start` 成功的模式；未启动时为 `NET_WIFI_MODE_OFF`。 */
net_wifi_mode_t net_wifi_get_mode(void);

/** STA 是否已取得 IPv4（事件位已置位）。 */
bool net_wifi_sta_has_ipv4(void);

/**
 * @brief 将当前界面可用的 IPv4 写入 buf。
 *        优先 STA DHCP 地址；否则 SoftAP 地址；Wi‑Fi 未就绪时为 "---"。
 * @return true 表示 buf 为有效 IPv4。
 */
bool net_wifi_format_ipv4_for_display(char *buf, size_t cap);

/** SoftAP 是否为当前运行模式（用于配网页写保护判断）。 */
bool net_wifi_is_softap_mode(void);

/**
 * @brief 客户端 IPv4（`struct in_addr.s_addr`，网络字节序）是否落在当前 SoftAP 接口子网内。
 * @note 非 SoftAP 或未配置 AP IP 时返回 false；用于 `/api/wifi/save` 等仅允许“连在 AP 上”的写操作。
 */
bool net_wifi_softap_peer_ipv4_on_ap_subnet(uint32_t addr_nbo);

/**
 * @brief 阻塞等待 STA 拿到 DHCP IPv4。
 * @note 仅 STA 模式有效；`WIFI_EVENT_STA_START` 会清除陈旧 GOT_IP 位。
 */
esp_err_t net_wifi_wait_sta_got_ip(uint32_t timeout_ms);
