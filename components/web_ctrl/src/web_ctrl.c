/**
 * @file web_ctrl.c
 * @brief Wi-Fi 与 HTTP 服务编排入口。
 */

#include "web_ctrl.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmd.h"
#include "usb_serial_jtag.h"

#include "nvs.h"

#include "web_ctrl_cmd.h"
#include "web_ctrl_wifi_api.h"
#include "web_server.h"

#ifndef CONFIG_WEB_CTRL_STA_CONNECT_TIMEOUT_MS
#define CONFIG_WEB_CTRL_STA_CONNECT_TIMEOUT_MS (15000)
#endif

static const char *TAG = "web_ctrl";

static bool s_web_ctrl_started;
static bool s_wifi_cmd_registered;

static void cmd_wifi(int argc, const char *argv[])
{
    if ((argc == 2) && (strcmp(argv[1], "sta_disconnect") == 0)) {
        const esp_err_t e = net_wifi_sta_disconnect();

        if (e != ESP_OK) {
            char buf[64];
            int  n = snprintf(buf, sizeof(buf), "ng wifi:%s\r\n", esp_err_to_name(e));

            if ((n > 0) && ((size_t)n < sizeof(buf))) {
                (void)cmd_send_str(buf);
            } else {
                cmd_reply_ng();
            }
            return;
        }
        if (!nvs_web_ctrl_settings_clear_sta_credentials()) {
            (void)cmd_send_str("ng wifi:nvs_clear_sta_failed\r\n");
            return;
        }
        cmd_reply_ok("wifi", "sta_disconnect_reboot");
        (void)UsbSerialJtagWaitTxDone((s32_t)200);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    cmd_reply_ng();
}

static void web_ctrl_register_wifi_cmd_once(void)
{
    if (s_wifi_cmd_registered) {
        return;
    }
    cmd_embed_register_defaults_if_needed();
    (void)cmd_register("wifi", cmd_wifi, "sta_disconnect");
    s_wifi_cmd_registered = true;
}

void web_ctrl_config_init_defaults(web_ctrl_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    (void)memset(cfg, 0, sizeof(*cfg));
    net_wifi_config_init_defaults(&cfg->wifi);
    cfg->http_port = 80U;
}

void web_ctrl_config_merge_nvs(web_ctrl_config_t *cfg)
{
    nvs_web_ctrl_settings_t st;

    if (cfg == NULL) {
        return;
    }
    if (!nvs_web_ctrl_settings_get(&st)) {
        return;
    }

    (void)strncpy(cfg->wifi.softap_ssid, st.softap_ssid, sizeof(cfg->wifi.softap_ssid) - 1U);
    cfg->wifi.softap_ssid[sizeof(cfg->wifi.softap_ssid) - 1U] = '\0';
    (void)strncpy(cfg->wifi.softap_password, st.softap_password, sizeof(cfg->wifi.softap_password) - 1U);
    cfg->wifi.softap_password[sizeof(cfg->wifi.softap_password) - 1U] = '\0';

    cfg->wifi.softap_channel = st.softap_channel;
    if (cfg->wifi.softap_channel == 0U) {
        cfg->wifi.softap_channel = 1U;
    }
    cfg->wifi.softap_max_connection = st.softap_max_connection;
    if (cfg->wifi.softap_max_connection == 0U) {
        cfg->wifi.softap_max_connection = 4U;
    }
    cfg->http_port = (st.http_port == 0U) ? 80U : st.http_port;

    (void)strncpy(cfg->wifi.sta_ssid, st.sta_ssid, sizeof(cfg->wifi.sta_ssid) - 1U);
    cfg->wifi.sta_ssid[sizeof(cfg->wifi.sta_ssid) - 1U] = '\0';
    (void)strncpy(cfg->wifi.sta_password, st.sta_password, sizeof(cfg->wifi.sta_password) - 1U);
    cfg->wifi.sta_password[sizeof(cfg->wifi.sta_password) - 1U] = '\0';
}

esp_err_t web_ctrl_start(const web_ctrl_config_t *cfg_in)
{
    web_ctrl_config_t cfg;
    esp_err_t         err;

    if (cfg_in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_web_ctrl_started) {
        ESP_LOGW(TAG, "web_ctrl already started");
        return ESP_ERR_INVALID_STATE;
    }

    cfg = *cfg_in;

    {
        const bool try_sta = (cfg.wifi.sta_ssid[0] != '\0');

        if (try_sta) {
            cfg.wifi.mode = NET_WIFI_MODE_STA;
            err           = net_wifi_start(&cfg.wifi);
            if (err == ESP_OK) {
                err = net_wifi_wait_sta_got_ip((uint32_t)CONFIG_WEB_CTRL_STA_CONNECT_TIMEOUT_MS);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "STA connected, starting HTTP");
                    goto start_http_stack;
                }
            }
            ESP_LOGW(TAG, "STA not ready (%s), SoftAP fallback", esp_err_to_name(err));
            (void)net_wifi_stop();
            cfg             = *cfg_in;
            cfg.wifi.mode = NET_WIFI_MODE_SOFTAP;
            err             = net_wifi_start(&cfg.wifi);
            if (err != ESP_OK) {
                return err;
            }
        } else {
            err = net_wifi_start(&cfg.wifi);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

start_http_stack:
    {
        const uint16_t port = (cfg.http_port == 0U) ? 80U : cfg.http_port;

        err = web_ctrl_cmd_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "web_ctrl_cmd_start failed, stopping Wi-Fi: %s", esp_err_to_name(err));
            (void)net_wifi_stop();
            return err;
        }

        err = web_server_start(port, cfg.root_get_handler);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "web_server_start failed, stopping Wi-Fi: %s", esp_err_to_name(err));
            (void)web_ctrl_cmd_stop();
            (void)net_wifi_stop();
            return err;
        }

        if (cfg.gallery_http_register != NULL) {
            err = cfg.gallery_http_register(web_server_get_handle());
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "gallery HTTP register failed: %s", esp_err_to_name(err));
                (void)web_server_stop();
                (void)web_ctrl_cmd_stop();
                (void)net_wifi_stop();
                return err;
            }
        }

        if (cfg.video_http_register != NULL) {
            err = cfg.video_http_register(web_server_get_handle());
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "video HTTP register failed: %s", esp_err_to_name(err));
                (void)web_server_stop();
                (void)web_ctrl_cmd_stop();
                (void)net_wifi_stop();
                return err;
            }
        }

        err = web_ctrl_wifi_api_register(web_server_get_handle());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "web_ctrl_wifi_api_register failed: %s", esp_err_to_name(err));
            (void)web_server_stop();
            (void)web_ctrl_cmd_stop();
            (void)net_wifi_stop();
            return err;
        }

        s_web_ctrl_started = true;
        web_ctrl_register_wifi_cmd_once();
        ESP_LOGI(TAG, "web_ctrl started (HTTP port %u)", (unsigned int)port);
    }
    return ESP_OK;
}

esp_err_t web_ctrl_stop(void)
{
    (void)web_server_stop();
    web_ctrl_cmd_stop();
    (void)net_wifi_stop();
    if (s_web_ctrl_started) {
        ESP_LOGI(TAG, "web_ctrl stopped");
    }
    s_web_ctrl_started = false;
    return ESP_OK;
}

bool web_ctrl_is_running(void)
{
    return s_web_ctrl_started;
}
