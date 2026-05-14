/**
 * @file    persist.c
 * @brief   非易失参数实现：ESP-IDF NVS Flash，API 与 STM32 自定义 Flash NVS 对齐。
 */

#include "persist.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *const TAG = "persist";

#define NVS_KEY_REBOOT_COUNT "rcount"
#define NVS_KEY_FRAME_COUNT "fcount"
#define NVS_KEY_HEARTBEAT_SEQ "hseq"
#define NVS_KEY_MAC "mac"
#define NVS_KEY_SN "sn"
#define NVS_KEY_REGION "region"
#define NVS_KEY_DEVICE_TYPE "dtype"
#define NVS_KEY_RUN_TIME "rtime"
#define NVS_KEY_LAST_HOURLY_RECORD "lhr"
#define NVS_KEY_BUZZ_DUR "buzz_dur"
#define NVS_KEY_ATT_LEVEL "att_lvl"
#define NVS_KEY_WEB_CTRL "web_ctrl"

nvs_ctx_t g_nvs_handle;

static nvs_handle_t s_h = 0;

static nvs_status_t err_to_nvs_status(esp_err_t e)
{
    switch (e) {
    case ESP_OK:
        return NVS_OK;
    case ESP_ERR_NVS_NOT_FOUND:
        return NVS_ERROR_NOT_FOUND;
    case ESP_ERR_NVS_NO_FREE_PAGES:
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
    case ESP_ERR_NVS_PAGE_FULL:
        return NVS_ERROR_NO_SPACE;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_NVS_INVALID_HANDLE:
        return NVS_ERROR_INVALID_PARAM;
    case ESP_ERR_NVS_INVALID_STATE:
        return NVS_ERROR_INVALID_STATE;
    case ESP_ERR_NVS_REMOVE_FAILED:
    case ESP_ERR_NVS_READ_ONLY:
        return NVS_ERROR_FLASH_WRITE;
    default:
        return NVS_ERROR_INVALID_STATE;
    }
}

static bool key_valid(const char *key)
{
    size_t len;

    if (key == NULL) {
        return false;
    }
    len = strlen(key);
    if (len == 0U || len >= NVS_KEY_MAX_LEN) {
        return false;
    }
    return true;
}

static void nvs_copy_version_string(char *buf, size_t buf_size, const char *src)
{
    if ((buf == NULL) || (buf_size == 0U) || (src == NULL)) {
        return;
    }
    (void)snprintf(buf, buf_size, "%s", src);
}

static esp_err_t ensure_open(void)
{
    esp_err_t err;

    if (s_h != 0) {
        return ESP_OK;
    }
    err = nvs_open(NVS_APP_NAMESPACE, NVS_READWRITE, &s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_APP_NAMESPACE, esp_err_to_name(err));
    }
    return err;
}

nvs_status_t nvs_set(nvs_ctx_t *nvs, const char *key, const void *value, size_t value_len)
{
    esp_err_t err;

    if ((nvs == NULL) || !key_valid(key) || (value == NULL) || (value_len == 0U) ||
        (value_len > NVS_VALUE_MAX_LEN)) {
        return NVS_ERROR_INVALID_PARAM;
    }
    if (!nvs->initialized) {
        return NVS_ERROR_NOT_INITIALIZED;
    }

    err = ensure_open();
    if (err != ESP_OK) {
        return err_to_nvs_status(err);
    }

    err = nvs_set_blob(s_h, key, value, value_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %s", key, esp_err_to_name(err));
        return err_to_nvs_status(err);
    }
    err = nvs_commit(s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit after set %s failed: %s", key, esp_err_to_name(err));
        return err_to_nvs_status(err);
    }
    return NVS_OK;
}

nvs_status_t nvs_get(nvs_ctx_t *nvs, const char *key, void *value, size_t *value_len)
{
    esp_err_t err;
    size_t need = 0;

    if ((nvs == NULL) || !key_valid(key) || (value == NULL) || (value_len == NULL)) {
        return NVS_ERROR_INVALID_PARAM;
    }
    if (!nvs->initialized) {
        return NVS_ERROR_NOT_INITIALIZED;
    }

    err = ensure_open();
    if (err != ESP_OK) {
        return err_to_nvs_status(err);
    }

    need = *value_len;
    err = nvs_get_blob(s_h, key, value, &need);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return NVS_ERROR_NOT_FOUND;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        *value_len = need;
        return NVS_ERROR_INVALID_PARAM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(%s) failed: %s", key, esp_err_to_name(err));
        return err_to_nvs_status(err);
    }
    *value_len = need;
    return NVS_OK;
}

nvs_status_t nvs_delete(nvs_ctx_t *nvs, const char *key)
{
    esp_err_t err;

    if ((nvs == NULL) || !key_valid(key)) {
        return NVS_ERROR_INVALID_PARAM;
    }
    if (!nvs->initialized) {
        return NVS_ERROR_NOT_INITIALIZED;
    }

    err = ensure_open();
    if (err != ESP_OK) {
        return err_to_nvs_status(err);
    }

    err = nvs_erase_key(s_h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return NVS_ERROR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_key(%s) failed: %s", key, esp_err_to_name(err));
        return err_to_nvs_status(err);
    }
    err = nvs_commit(s_h);
    if (err != ESP_OK) {
        return err_to_nvs_status(err);
    }
    return NVS_OK;
}

static void nvs_print_boot_info(void)
{
    uint32_t reboot_count = nvs_reboot_count_get();
    char sn_buf[NVS_SN_SIZE];
    char hw_buf[NVS_HW_VERSION_SIZE];
    char region_buf[NVS_REGION_SIZE];

    LOG_INFO("--- NVS boot ---");
    LOG_INFO("  reboot_count: %lu", (unsigned long)reboot_count);
    if (nvs_sn_get(sn_buf)) {
        LOG_INFO("  sn: %s", sn_buf);
    }
    if (nvs_hw_version_get(hw_buf)) {
        LOG_INFO("  hw: %s", hw_buf);
    }
    {
        char app_buf[NVS_APP_VERSION_SIZE];
        if (nvs_app_version_get(app_buf)) {
            LOG_INFO("  app: %s", app_buf);
        }
    }
    {
        char ftm_buf[NVS_FACTORY_VERSION_SIZE];
        if (nvs_factory_version_get(ftm_buf)) {
            LOG_INFO("  factory: %s", ftm_buf);
        }
    }
    if (nvs_region_get(region_buf)) {
        LOG_INFO("  region: %s", region_buf);
    }
    LOG_INFO("  device_type: %u", (unsigned int)nvs_device_type_get());
    {
        nvs_web_ctrl_settings_t wc;
        if (nvs_web_ctrl_settings_get(&wc)) {
            LOG_INFO("  web_ctrl: ap_ssid=%s ch=%u max=%u port=%u sta_ssid=%s", wc.softap_ssid,
                     (unsigned int)wc.softap_channel, (unsigned int)wc.softap_max_connection, (unsigned int)wc.http_port,
                     (wc.sta_ssid[0] != '\0') ? wc.sta_ssid : "-");
        }
    }
    LOG_INFO("----------------");
}

static void nvs_ensure_defaults(void)
{
    char sn_buf[NVS_SN_SIZE];

    if (!nvs_sn_get(sn_buf)) {
        (void)nvs_sn_set(NVS_DEFAULT_SN);
    }

    {
        char region_buf[NVS_REGION_SIZE];
        if (!nvs_region_get(region_buf)) {
            (void)nvs_region_set(NVS_DEFAULT_REGION);
        }
    }

    {
        uint8_t type = 0;
        size_t len = sizeof(type);
        if (nvs_get(&g_nvs_handle, NVS_KEY_DEVICE_TYPE, &type, &len) != NVS_OK) {
            nvs_device_type_set((uint8_t)NVS_DEFAULT_DEVICE_TYPE);
        }
    }
}

void nvs_init(void)
{
    esp_err_t err;

    if (g_nvs_handle.initialized) {
        return;
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init returned %s, erase and retry", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
            return;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = ensure_open();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open namespace failed");
        return;
    }

    memset(&g_nvs_handle, 0, sizeof(g_nvs_handle));
    g_nvs_handle.initialized = true;

    nvs_ensure_defaults();
    {
        uint32_t count = nvs_reboot_count_get();
        count++;
        nvs_reboot_count_set(count);
    }
    LOG_INFO("NVS initialized");
    nvs_print_boot_info();
}

void nvs_factory_reset(void)
{
    esp_err_t err;

    if (!g_nvs_handle.initialized) {
        ESP_LOGE(TAG, "factory_reset: not initialized");
        return;
    }

    err = ensure_open();
    if (err != ESP_OK) {
        return;
    }

    err = nvs_erase_all(s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_all failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_commit(s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit after erase_all failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_ensure_defaults();
    ESP_LOGI(TAG, "factory reset completed");
}

void nvs_reboot_count_set(uint32_t count)
{
    nvs_status_t st;

    if (!g_nvs_handle.initialized) {
        LOG_WARN("NVS: not initialized, cannot set reboot count");
        return;
    }
    st = nvs_set(&g_nvs_handle, NVS_KEY_REBOOT_COUNT, &count, sizeof(uint32_t));
    if (st != NVS_OK) {
        LOG_ERROR("NVS: set reboot count failed, status=%d", (int)st);
    }
}

uint32_t nvs_reboot_count_get(void)
{
    uint32_t count = 0;
    size_t len = sizeof(uint32_t);

    if (!g_nvs_handle.initialized) {
        return 0U;
    }
    if (nvs_get(&g_nvs_handle, NVS_KEY_REBOOT_COUNT, &count, &len) != NVS_OK) {
        return 0U;
    }
    return count;
}

void nvs_frame_count_set(uint32_t count)
{
    if (!g_nvs_handle.initialized) {
        return;
    }
    (void)nvs_set(&g_nvs_handle, NVS_KEY_FRAME_COUNT, &count, sizeof(uint32_t));
}

uint32_t nvs_frame_count_get(void)
{
    uint32_t count = 0;
    size_t len = sizeof(uint32_t);

    if (!g_nvs_handle.initialized) {
        return 0U;
    }
    if (nvs_get(&g_nvs_handle, NVS_KEY_FRAME_COUNT, &count, &len) != NVS_OK) {
        return 0U;
    }
    return count;
}

void nvs_heartbeat_seq_set(uint32_t seq)
{
    if (!g_nvs_handle.initialized) {
        return;
    }
    (void)nvs_set(&g_nvs_handle, NVS_KEY_HEARTBEAT_SEQ, &seq, sizeof(uint32_t));
}

uint32_t nvs_heartbeat_seq_get(void)
{
    uint32_t seq = 0;
    size_t len = sizeof(uint32_t);

    if (!g_nvs_handle.initialized) {
        return 0U;
    }
    if (nvs_get(&g_nvs_handle, NVS_KEY_HEARTBEAT_SEQ, &seq, &len) != NVS_OK) {
        return 0U;
    }
    return seq;
}

bool nvs_mac_set(const uint8_t *mac)
{
    if (!g_nvs_handle.initialized || (mac == NULL)) {
        return false;
    }
    return (nvs_set(&g_nvs_handle, NVS_KEY_MAC, mac, NVS_MAC_SIZE) == NVS_OK);
}

bool nvs_mac_get(uint8_t *mac)
{
    size_t len = NVS_MAC_SIZE;

    if (!g_nvs_handle.initialized || (mac == NULL)) {
        return false;
    }
    return (nvs_get(&g_nvs_handle, NVS_KEY_MAC, mac, &len) == NVS_OK);
}

bool nvs_sn_set(const char *sn)
{
    if (!g_nvs_handle.initialized || (sn == NULL)) {
        return false;
    }
    if (strlen(sn) >= NVS_SN_SIZE) {
        LOG_ERROR("NVS: SN too long");
        return false;
    }
    return (nvs_set(&g_nvs_handle, NVS_KEY_SN, sn, strlen(sn) + 1U) == NVS_OK);
}

bool nvs_sn_get(char *sn)
{
    size_t len = NVS_SN_SIZE;

    if (!g_nvs_handle.initialized || (sn == NULL)) {
        return false;
    }
    if (nvs_get(&g_nvs_handle, NVS_KEY_SN, sn, &len) == NVS_OK) {
        sn[NVS_SN_SIZE - 1U] = '\0';
        return true;
    }
    return false;
}

bool nvs_hw_version_get(char *version)
{
    if (version == NULL) {
        return false;
    }
    nvs_copy_version_string(version, NVS_HW_VERSION_SIZE, NVS_HW_VERSION_STRING);
    return true;
}

bool nvs_app_version_get(char *version)
{
    if (version == NULL) {
        return false;
    }
    nvs_copy_version_string(version, NVS_APP_VERSION_SIZE, NVS_APP_VERSION_STRING);
    return true;
}

bool nvs_factory_version_get(char *version)
{
    if (version == NULL) {
        return false;
    }
    nvs_copy_version_string(version, NVS_FACTORY_VERSION_SIZE, NVS_FACTORY_VERSION_STRING);
    return true;
}

bool nvs_region_set(const char *region)
{
    if (!g_nvs_handle.initialized || (region == NULL)) {
        return false;
    }
    if (strlen(region) >= NVS_REGION_SIZE) {
        LOG_ERROR("NVS: region too long");
        return false;
    }
    return (nvs_set(&g_nvs_handle, NVS_KEY_REGION, region, strlen(region) + 1U) == NVS_OK);
}

bool nvs_region_get(char *region)
{
    size_t len = NVS_REGION_SIZE;

    if (!g_nvs_handle.initialized || (region == NULL)) {
        return false;
    }
    if (nvs_get(&g_nvs_handle, NVS_KEY_REGION, region, &len) == NVS_OK) {
        region[NVS_REGION_SIZE - 1U] = '\0';
        return true;
    }
    return false;
}

void nvs_device_type_set(uint8_t type)
{
    if (!g_nvs_handle.initialized) {
        return;
    }
    (void)nvs_set(&g_nvs_handle, NVS_KEY_DEVICE_TYPE, &type, sizeof(uint8_t));
}

uint8_t nvs_device_type_get(void)
{
    uint8_t type = 0;
    size_t len = sizeof(uint8_t);

    if (!g_nvs_handle.initialized) {
        return (uint8_t)NVS_DEFAULT_DEVICE_TYPE;
    }
    if (nvs_get(&g_nvs_handle, NVS_KEY_DEVICE_TYPE, &type, &len) != NVS_OK) {
        return (uint8_t)NVS_DEFAULT_DEVICE_TYPE;
    }
    return type;
}

void nvs_run_time_set(uint32_t sec)
{
    if (!g_nvs_handle.initialized) {
        return;
    }
    (void)nvs_set(&g_nvs_handle, NVS_KEY_RUN_TIME, &sec, sizeof(uint32_t));
}

uint32_t nvs_run_time_get(void)
{
    uint32_t sec = NVS_DEFAULT_RUN_TIME;
    size_t len = sizeof(uint32_t);

    if (!g_nvs_handle.initialized) {
        return NVS_DEFAULT_RUN_TIME;
    }
    if (nvs_get(&g_nvs_handle, NVS_KEY_RUN_TIME, &sec, &len) != NVS_OK) {
        return NVS_DEFAULT_RUN_TIME;
    }
    return sec;
}

void nvs_last_hourly_record_set(uint32_t sec)
{
    if (!g_nvs_handle.initialized) {
        return;
    }
    (void)nvs_set(&g_nvs_handle, NVS_KEY_LAST_HOURLY_RECORD, &sec, sizeof(uint32_t));
}

uint32_t nvs_last_hourly_record_get(void)
{
    uint32_t sec = NVS_DEFAULT_LAST_HOURLY_RECORD;
    size_t len = sizeof(uint32_t);

    if (!g_nvs_handle.initialized) {
        return NVS_DEFAULT_LAST_HOURLY_RECORD;
    }
    if (nvs_get(&g_nvs_handle, NVS_KEY_LAST_HOURLY_RECORD, &sec, &len) != NVS_OK) {
        return NVS_DEFAULT_LAST_HOURLY_RECORD;
    }
    return sec;
}

bool nvs_buzzer_duration_sec_set(uint32_t sec)
{
    if (!g_nvs_handle.initialized) {
        return false;
    }
    if ((sec < 1U) || (sec > 3600U)) {
        return false;
    }
    return (nvs_set(&g_nvs_handle, NVS_KEY_BUZZ_DUR, &sec, sizeof(sec)) == NVS_OK);
}

bool nvs_buzzer_duration_sec_get(uint32_t *out)
{
    uint32_t v = 0U;
    size_t len = sizeof(v);

    if (!g_nvs_handle.initialized || (out == NULL)) {
        return false;
    }
    if ((nvs_get(&g_nvs_handle, NVS_KEY_BUZZ_DUR, &v, &len) != NVS_OK) || (len != sizeof(v))) {
        return false;
    }
    if ((v < 1U) || (v > 3600U)) {
        return false;
    }
    *out = v;
    return true;
}

bool nvs_attitude_level_cal_set(const nvs_attitude_level_cal_t *cal)
{
    if (!g_nvs_handle.initialized || (cal == NULL)) {
        return false;
    }
    if (cal->magic != NVS_ATT_LEVEL_MAGIC) {
        return false;
    }
    return (nvs_set(&g_nvs_handle, NVS_KEY_ATT_LEVEL, cal, sizeof(*cal)) == NVS_OK);
}

bool nvs_attitude_level_cal_get(nvs_attitude_level_cal_t *out)
{
    size_t len = sizeof(*out);

    if (!g_nvs_handle.initialized || (out == NULL)) {
        return false;
    }
    if ((nvs_get(&g_nvs_handle, NVS_KEY_ATT_LEVEL, out, &len) != NVS_OK) || (len != sizeof(*out))) {
        return false;
    }
    if (out->magic != NVS_ATT_LEVEL_MAGIC) {
        return false;
    }
    return true;
}

bool nvs_attitude_level_cal_delete(void)
{
    nvs_status_t st;

    if (!g_nvs_handle.initialized) {
        return false;
    }
    st = nvs_delete(&g_nvs_handle, NVS_KEY_ATT_LEVEL);
    return (st == NVS_OK) || (st == NVS_ERROR_NOT_FOUND);
}

void nvs_web_ctrl_settings_default(nvs_web_ctrl_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    (void)memset(out, 0, sizeof(*out));
    out->magic = NVS_WEB_CTRL_MAGIC;
    (void)strncpy(out->softap_ssid, "ESP32-WebCtrl", sizeof(out->softap_ssid) - 1U);
    out->softap_ssid[sizeof(out->softap_ssid) - 1U] = '\0';
    (void)strncpy(out->softap_password, "esp32web1", sizeof(out->softap_password) - 1U);
    out->softap_password[sizeof(out->softap_password) - 1U] = '\0';
    out->softap_channel = 1U;
    out->softap_max_connection = 4U;
    out->http_port = 80U;
}

bool nvs_web_ctrl_settings_validate(const nvs_web_ctrl_settings_t *cfg)
{
    size_t ssid_len;
    size_t pass_len;
    size_t sta_ssid_len;
    size_t sta_pass_len;

    if (cfg == NULL) {
        return false;
    }
    if (cfg->magic != NVS_WEB_CTRL_MAGIC) {
        return false;
    }
    ssid_len = strnlen(cfg->softap_ssid, sizeof(cfg->softap_ssid));
    if ((ssid_len == 0U) || (ssid_len > 32U)) {
        return false;
    }
    pass_len = strnlen(cfg->softap_password, sizeof(cfg->softap_password));
    if ((pass_len > 0U) && (pass_len < 8U)) {
        return false;
    }
    if ((cfg->softap_channel > 13U) && (cfg->softap_channel != 0U)) {
        return false;
    }
    if (cfg->softap_max_connection > 10U) {
        return false;
    }
    sta_ssid_len = strnlen(cfg->sta_ssid, sizeof(cfg->sta_ssid));
    if (sta_ssid_len > 32U) {
        return false;
    }
    sta_pass_len = strnlen(cfg->sta_password, sizeof(cfg->sta_password));
    if (sta_pass_len > 64U) {
        return false;
    }
    return true;
}

/** NVS `web_ctrl` 旧版 blob 尺寸（无 STA 字段），用于迁移读。 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    char softap_ssid[33];
    char softap_password[65];
    uint8_t softap_channel;
    uint8_t softap_max_connection;
    uint16_t http_port;
    uint8_t reserved[2];
} nvs_web_ctrl_settings_v1_t;

_Static_assert(sizeof(nvs_web_ctrl_settings_v1_t) == 108U, "nvs_web_ctrl v1 size");
_Static_assert(sizeof(nvs_web_ctrl_settings_t) <= NVS_VALUE_MAX_LEN, "web_ctrl blob must fit NVS_VALUE_MAX_LEN");

bool nvs_web_ctrl_settings_get(nvs_web_ctrl_settings_t *out)
{
    uint8_t buf[sizeof(nvs_web_ctrl_settings_t)];
    size_t len = sizeof(buf);

    if (!g_nvs_handle.initialized || (out == NULL)) {
        return false;
    }
    if (nvs_get(&g_nvs_handle, NVS_KEY_WEB_CTRL, buf, &len) != NVS_OK) {
        return false;
    }
    if (len == sizeof(nvs_web_ctrl_settings_v1_t)) {
        nvs_web_ctrl_settings_v1_t v1;

        (void)memcpy(&v1, buf, sizeof(v1));
        (void)memset(out, 0, sizeof(*out));
        (void)memcpy(out, &v1, sizeof(v1));
        out->sta_ssid[0]     = '\0';
        out->sta_password[0] = '\0';
    } else if (len == sizeof(nvs_web_ctrl_settings_t)) {
        (void)memcpy(out, buf, sizeof(*out));
    } else {
        return false;
    }
    if (!nvs_web_ctrl_settings_validate(out)) {
        return false;
    }
    return true;
}

bool nvs_web_ctrl_settings_set(const nvs_web_ctrl_settings_t *cfg)
{
    if (!g_nvs_handle.initialized || (cfg == NULL)) {
        return false;
    }
    if (!nvs_web_ctrl_settings_validate(cfg)) {
        return false;
    }
    return (nvs_set(&g_nvs_handle, NVS_KEY_WEB_CTRL, cfg, sizeof(*cfg)) == NVS_OK);
}

bool nvs_web_ctrl_settings_clear_sta_credentials(void)
{
    nvs_web_ctrl_settings_t st;

    if (!g_nvs_handle.initialized) {
        return false;
    }
    if (!nvs_web_ctrl_settings_get(&st)) {
        nvs_web_ctrl_settings_default(&st);
    }
    (void)memset(st.sta_ssid, 0, sizeof(st.sta_ssid));
    (void)memset(st.sta_password, 0, sizeof(st.sta_password));
    st.magic = NVS_WEB_CTRL_MAGIC;
    if (!nvs_web_ctrl_settings_validate(&st)) {
        return false;
    }
    return nvs_web_ctrl_settings_set(&st);
}

bool nvs_web_ctrl_settings_delete(void)
{
    nvs_status_t st;

    if (!g_nvs_handle.initialized) {
        return false;
    }
    st = nvs_delete(&g_nvs_handle, NVS_KEY_WEB_CTRL);
    return (st == NVS_OK) || (st == NVS_ERROR_NOT_FOUND);
}
