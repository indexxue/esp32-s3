/**
 * @file    nvs.c
 * @brief   应用层非易失参数实现（ESP-IDF NVS Flash）。
 */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "log.h"
#include "boot_slot.h"

/* ESP-IDF NVS 错误码（无法 #include 官方 nvs.h，会与 common/inc/nvs.h 冲突） */
#ifndef ESP_ERR_NVS_BASE
#define ESP_ERR_NVS_BASE 0x1100
#endif
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_READ_ONLY (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_INVALID_HANDLE (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_REMOVE_FAILED (ESP_ERR_NVS_BASE + 0x08)
#define ESP_ERR_NVS_PAGE_FULL (ESP_ERR_NVS_BASE + 0x0a)
#define ESP_ERR_NVS_INVALID_STATE (ESP_ERR_NVS_BASE + 0x0b)
#define ESP_ERR_NVS_INVALID_LENGTH (ESP_ERR_NVS_BASE + 0x0c)
#define ESP_ERR_NVS_NO_FREE_PAGES (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)

/* ESP-IDF NVS 底层 API 前向声明 */
typedef uint32_t esp_nvs_handle_t;
typedef enum {
    ESP_NVS_READONLY = 0,
    ESP_NVS_READWRITE = 1,
} esp_nvs_open_mode_t;

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_open(const char *namespace_name, esp_nvs_open_mode_t open_mode, esp_nvs_handle_t *out_handle);
esp_err_t nvs_set_blob(esp_nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_get_blob(esp_nvs_handle_t handle, const char *key, void *out_value, size_t *length);
esp_err_t nvs_erase_key(esp_nvs_handle_t handle, const char *key);
esp_err_t nvs_erase_all(esp_nvs_handle_t handle);
esp_err_t nvs_commit(esp_nvs_handle_t handle);

#include "nvs.h"

/* 编译期固件构建日期：优先 CMake 注入 NVS_FIRMWARE_BUILD_DATE，否则解析 __DATE__ */
#ifndef NVS_FIRMWARE_BUILD_DATE
#define NVS_BUILD_MONTH_IS(c0, c1, c2) (__DATE__[0] == (c0) && __DATE__[1] == (c1) && __DATE__[2] == (c2))
#define NVS_BUILD_MONTH_NUM                                                                                          \
    (NVS_BUILD_MONTH_IS('J', 'a', 'n')   ? 1U  : NVS_BUILD_MONTH_IS('F', 'e', 'b') ? 2U  :                           \
     NVS_BUILD_MONTH_IS('M', 'a', 'r')   ? 3U  : NVS_BUILD_MONTH_IS('A', 'p', 'r') ? 4U  :                           \
     NVS_BUILD_MONTH_IS('M', 'a', 'y')   ? 5U  : NVS_BUILD_MONTH_IS('J', 'u', 'n') ? 6U  :                           \
     NVS_BUILD_MONTH_IS('J', 'u', 'l')   ? 7U  : NVS_BUILD_MONTH_IS('A', 'u', 'g') ? 8U  :                           \
     NVS_BUILD_MONTH_IS('S', 'e', 'p')   ? 9U  : NVS_BUILD_MONTH_IS('O', 'c', 't') ? 10U :                           \
     NVS_BUILD_MONTH_IS('N', 'o', 'v')   ? 11U : 12U)
#define NVS_BUILD_DAY_NUM                                                                                            \
    ((uint32_t)((__DATE__[4] == ' ' ? 0U : (uint32_t)(__DATE__[4] - '0')) * 10U + (uint32_t)(__DATE__[5] - '0')))
#define NVS_BUILD_YEAR_NUM                                                                                           \
    ((uint32_t)(__DATE__[7] - '0') * 1000U + (uint32_t)(__DATE__[8] - '0') * 100U +                                \
     (uint32_t)(__DATE__[9] - '0') * 10U + (uint32_t)(__DATE__[10] - '0'))
#define NVS_FIRMWARE_BUILD_DATE                                                                                      \
    (NVS_BUILD_YEAR_NUM * 10000U + NVS_BUILD_MONTH_NUM * 100U + NVS_BUILD_DAY_NUM)
#endif

static const char *const TAG = "nvs";

#define NVS_KEY_MAX_LEN 16
#define NVS_KEY_VALUE_MAX 256
#define NVS_DEVICE_NAME_SIZE 16
#define NVS_REGION_SIZE 8

#define NVS_KEY_REBOOT_COUNT "rcount"
#define NVS_KEY_MAC "mac"
#define NVS_KEY_SN "sn"
#define NVS_KEY_REGION "region"
#define NVS_KEY_DEVICE_TYPE "dtype"
#define NVS_KEY_DEVICE_ID "did"
#define NVS_KEY_HARDWARE_ID "hid"
#define NVS_KEY_DEVICE_NAME "dname"
#define NVS_KEY_BUILD_DATE "bdate"
#define NVS_KEY_APP_VERSION "appver"
#define NVS_KEY_FACTORY_VERSION "fver"
#define NVS_KEY_RUN_TIME "rtime"
#define NVS_KEY_WEB_CTRL "web_ctrl"
#define NVS_KEY_LCD_GAL_BOOT "lcd_gal_boot"
#define NVS_KEY_CAMERA_CFG "cam_cfg"
#define NVS_KEY_SERVO_CAL "servo_cal"
#define NVS_KEY_TOUCH_CAL "touch_cal"
#define NVS_KEY_PET_NEEDS "pet_needs"

/** 与 board.h 舵机映射一致；nvs 不依赖 board，避免层倒挂。 */
#define NVS_SERVO_CALIB_PULSE_MIN_US (500U)
#define NVS_SERVO_CALIB_PULSE_MAX_US (2500U)
#define NVS_SERVO_CALIB_CENTER_PULSE_US (1500U)
#define NVS_SERVO_CALIB_ANGLE_MAX_DEG (360.0f)
#define NVS_SERVO_CALIB_CENTER_DEG (180.0f)
#define NVS_SERVO_CALIB_OFFSET_ABS_MAX (180.0f)

static const uint8_t s_default_mac[NVS_MAC_SIZE] = NVS_DEFAULT_MAC;

static bool s_ready;
static esp_nvs_handle_t s_h;

typedef enum {
    NVS_OK = 0,
    NVS_ERR_PARAM,
    NVS_ERR_NOT_READY,
    NVS_ERR_NOT_FOUND,
    NVS_ERR_NO_SPACE,
    NVS_ERR_IO,
    NVS_ERR_STATE,
} nvs_err_t;

uint32_t nvs_firmware_build_date(void);
uint32_t nvs_build_date_get(void);
bool nvs_build_date_format(uint32_t ymd, char *out, size_t out_cap);

static bool key_valid(const char *key)
{
    size_t len;

    if (key == NULL) {
        return false;
    }
    len = strlen(key);
    return (len > 0U) && (len < NVS_KEY_MAX_LEN);
}

static nvs_err_t esp_to_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK:
        return NVS_OK;
    case ESP_ERR_NVS_NOT_FOUND:
        return NVS_ERR_NOT_FOUND;
    case ESP_ERR_NVS_NO_FREE_PAGES:
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
    case ESP_ERR_NVS_PAGE_FULL:
        return NVS_ERR_NO_SPACE;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_NVS_INVALID_HANDLE:
        return NVS_ERR_PARAM;
    case ESP_ERR_NVS_INVALID_STATE:
        return NVS_ERR_STATE;
    case ESP_ERR_NVS_REMOVE_FAILED:
    case ESP_ERR_NVS_READ_ONLY:
        return NVS_ERR_IO;
    default:
        return NVS_ERR_STATE;
    }
}

static esp_err_t ensure_open(void)
{
    esp_err_t err;

    if (s_h != 0) {
        return ESP_OK;
    }
    err = nvs_open(NVS_APP_NAMESPACE, ESP_NVS_READWRITE, &s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_APP_NAMESPACE, esp_err_to_name(err));
    }
    return err;
}

static nvs_err_t blob_set(const char *key, const void *value, size_t value_len)
{
    esp_err_t err;

    if (!key_valid(key) || (value == NULL) || (value_len == 0U) || (value_len > NVS_KEY_VALUE_MAX)) {
        return NVS_ERR_PARAM;
    }
    if (!s_ready) {
        return NVS_ERR_NOT_READY;
    }

    err = ensure_open();
    if (err != ESP_OK) {
        return esp_to_err(err);
    }

    err = nvs_set_blob(s_h, key, value, value_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_blob(%s) failed: %s", key, esp_err_to_name(err));
        return esp_to_err(err);
    }
    err = nvs_commit(s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit after set %s failed: %s", key, esp_err_to_name(err));
        return esp_to_err(err);
    }
    return NVS_OK;
}

static nvs_err_t blob_get(const char *key, void *value, size_t *value_len)
{
    esp_err_t err;
    size_t need;

    if (!key_valid(key) || (value == NULL) || (value_len == NULL)) {
        return NVS_ERR_PARAM;
    }
    if (!s_ready) {
        return NVS_ERR_NOT_READY;
    }

    err = ensure_open();
    if (err != ESP_OK) {
        return esp_to_err(err);
    }

    need = *value_len;
    err = nvs_get_blob(s_h, key, value, &need);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return NVS_ERR_NOT_FOUND;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        *value_len = need;
        return NVS_ERR_PARAM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_blob(%s) failed: %s", key, esp_err_to_name(err));
        return esp_to_err(err);
    }
    *value_len = need;
    return NVS_OK;
}

static nvs_err_t blob_del(const char *key)
{
    esp_err_t err;

    if (!key_valid(key)) {
        return NVS_ERR_PARAM;
    }
    if (!s_ready) {
        return NVS_ERR_NOT_READY;
    }

    err = ensure_open();
    if (err != ESP_OK) {
        return esp_to_err(err);
    }

    err = nvs_erase_key(s_h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return NVS_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase_key(%s) failed: %s", key, esp_err_to_name(err));
        return esp_to_err(err);
    }
    err = nvs_commit(s_h);
    return esp_to_err(err);
}

static bool read_u32(const char *key, uint32_t *out, uint32_t def)
{
    uint32_t v = def;
    size_t len = sizeof(v);

    if (!s_ready || (out == NULL)) {
        return false;
    }
    if (blob_get(key, &v, &len) != NVS_OK) {
        *out = def;
        return false;
    }
    *out = v;
    return true;
}

static bool write_u32(const char *key, uint32_t val)
{
    if (!s_ready) {
        return false;
    }
    return (blob_set(key, &val, sizeof(val)) == NVS_OK);
}

static bool read_u8(const char *key, uint8_t *out, uint8_t def)
{
    uint8_t v = def;
    size_t len = sizeof(v);

    if (!s_ready || (out == NULL)) {
        return false;
    }
    if (blob_get(key, &v, &len) != NVS_OK) {
        *out = def;
        return false;
    }
    *out = v;
    return true;
}

static bool write_u8(const char *key, uint8_t val)
{
    if (!s_ready) {
        return false;
    }
    return (blob_set(key, &val, sizeof(val)) == NVS_OK);
}

static bool write_str(const char *key, const char *s, size_t max_len)
{
    size_t n;

    if (!s_ready || (s == NULL)) {
        return false;
    }
    n = strlen(s);
    if ((n == 0U) || (n >= max_len)) {
        return false;
    }
    return (blob_set(key, s, n + 1U) == NVS_OK);
}

static bool read_str(const char *key, char *out, size_t cap)
{
    size_t len;

    if (!s_ready || (out == NULL) || (cap == 0U)) {
        return false;
    }
    len = cap;
    if (blob_get(key, out, &len) != NVS_OK) {
        return false;
    }
    out[cap - 1U] = '\0';
    return true;
}

static void copy_version(char *buf, size_t cap, const char *src)
{
    if ((buf == NULL) || (cap == 0U) || (src == NULL)) {
        return;
    }
    (void)snprintf(buf, cap, "%s", src);
}

static const char *project_name_for_id(uint32_t id)
{
    switch (id) {
#define NVS_PROJECT_NAME_CASE(name, val, dname)                                                                      \
    case (val):                                                                                                      \
        return dname;
        NVS_PROJECT_ID_TABLE(NVS_PROJECT_NAME_CASE)
#undef NVS_PROJECT_NAME_CASE
    default:
        return NULL;
    }
}

static bool device_name_set(const char *name)
{
    return write_str(NVS_KEY_DEVICE_NAME, name, NVS_DEVICE_NAME_SIZE);
}

static bool device_name_apply(uint32_t id)
{
    const char *proj = project_name_for_id(id);

    if (proj != NULL) {
        return device_name_set(proj);
    }
    if (id == NVS_DEVICE_ID_DEFAULT) {
        return device_name_set(NVS_DEVICE_NAME_DEFAULT);
    }
    return false;
}

static bool build_date_valid(uint32_t ymd)
{
    uint32_t year = ymd / 10000U;
    uint32_t month = (ymd / 100U) % 100U;
    uint32_t day = ymd % 100U;

    if ((year < 2020U) || (year > 2099U) || (month < 1U) || (month > 12U) || (day < 1U) || (day > 31U)) {
        return false;
    }
    return true;
}

static void sync_build_date(void)
{
    uint32_t stored = 0U;
    uint32_t fw = nvs_firmware_build_date();

    if (!build_date_valid(fw)) {
        LOG_WARN("NVS: invalid firmware build date %lu", (unsigned long)fw);
        return;
    }
    if (!read_u32(NVS_KEY_BUILD_DATE, &stored, 0U) || (stored != fw)) {
        (void)write_u32(NVS_KEY_BUILD_DATE, fw);
    }
}

static void sync_version_string(const char *key, size_t max_len, const char *fw_ver)
{
    char stored[NVS_APP_VERSION_SIZE] = {0};

    if ((key == NULL) || (fw_ver == NULL) || (fw_ver[0] == '\0') || (max_len == 0U)) {
        return;
    }
    if (max_len > sizeof(stored)) {
        max_len = sizeof(stored);
    }
    if (!read_str(key, stored, max_len) || (strcmp(stored, fw_ver) != 0)) {
        if (stored[0] != '\0') {
            LOG_WARN("NVS %s '%s' -> '%s'", key, stored, fw_ver);
        }
        (void)write_str(key, fw_ver, max_len);
    }
}

static void sync_app_version(void)
{
    sync_version_string(NVS_KEY_APP_VERSION, NVS_APP_VERSION_SIZE, NVS_APP_VERSION_STRING);
}

static void sync_factory_version(void)
{
    sync_version_string(NVS_KEY_FACTORY_VERSION, NVS_FACTORY_VERSION_SIZE, NVS_FACTORY_VERSION_STRING);
}

static void sync_firmware_product_id(void);

static void ensure_defaults(void)
{
#ifdef BUILD_FACTORY
    bool overwrite = false;
#endif
    char sn[NVS_SN_SIZE];
    uint8_t mac[NVS_MAC_SIZE];
    char region[NVS_REGION_SIZE];
    uint8_t type;
    uint32_t dev_id;

#ifdef BUILD_FACTORY
#ifndef FACTORY_OVERWRITE_NVS
#define FACTORY_OVERWRITE_NVS 0
#endif
#if FACTORY_OVERWRITE_NVS
    overwrite = true;
#endif
    if (overwrite || !nvs_sn_get(sn)) {
        (void)nvs_sn_set(NVS_DEFAULT_SN);
    }
    if (overwrite || !nvs_mac_get(mac)) {
        (void)nvs_mac_set(s_default_mac);
    }
    if (overwrite || !read_str(NVS_KEY_REGION, region, sizeof(region))) {
        (void)write_str(NVS_KEY_REGION, NVS_DEFAULT_REGION, NVS_REGION_SIZE);
    }
    if (overwrite || !read_u8(NVS_KEY_DEVICE_TYPE, &type, 0)) {
        nvs_device_type_set((uint8_t)NVS_DEFAULT_DEVICE_TYPE);
    }
    if (overwrite || !read_u32(NVS_KEY_DEVICE_ID, &dev_id, 0)) {
        (void)nvs_device_id_set((uint32_t)NVS_DEFAULT_DEVICE_ID);
    }
    {
        uint32_t hw_id = 0U;
        if (overwrite || !read_u32(NVS_KEY_HARDWARE_ID, &hw_id, 0)) {
            (void)nvs_hardware_id_set((uint32_t)NVS_DEFAULT_HARDWARE_ID);
        }
    }
#else
    if (!nvs_sn_get(sn)) {
        (void)nvs_sn_set(NVS_DEFAULT_SN);
    }
    if (!nvs_mac_get(mac)) {
        (void)nvs_mac_set(s_default_mac);
    }
    if (!read_str(NVS_KEY_REGION, region, sizeof(region))) {
        (void)write_str(NVS_KEY_REGION, NVS_DEFAULT_REGION, NVS_REGION_SIZE);
    }
    if (!read_u8(NVS_KEY_DEVICE_TYPE, &type, 0)) {
        nvs_device_type_set((uint8_t)NVS_DEFAULT_DEVICE_TYPE);
    }
    if (!read_u32(NVS_KEY_DEVICE_ID, &dev_id, 0)) {
        (void)nvs_device_id_set((uint32_t)NVS_DEFAULT_DEVICE_ID);
    }
    {
        uint32_t hw_id = 0U;
        if (!read_u32(NVS_KEY_HARDWARE_ID, &hw_id, 0)) {
            (void)nvs_hardware_id_set((uint32_t)NVS_DEFAULT_HARDWARE_ID);
        }
    }
#endif

    sync_firmware_product_id();
    (void)device_name_apply(nvs_device_id_get());
    sync_build_date();
    sync_app_version();
    sync_factory_version();
}

static void sync_firmware_product_id(void)
{
    const uint32_t fw = (uint32_t)NVS_DEFAULT_DEVICE_ID;
    uint32_t stored;

    if (fw == NVS_DEVICE_ID_DEFAULT) {
        return;
    }
    stored = nvs_device_id_get();
    if (stored != fw) {
        LOG_WARN("NVS product_id 0x%08lX != firmware 0x%08lX, syncing to firmware",
                 (unsigned long)stored, (unsigned long)fw);
        (void)nvs_device_id_set(fw);
    }
}

static void log_boot_info(void)
{
    char sn[NVS_SN_SIZE];
    char hw[NVS_HW_VERSION_SIZE];
    char region[NVS_REGION_SIZE];
    char name[NVS_DEVICE_NAME_SIZE];
    char boot_slot[48];
    uint8_t mac[NVS_MAC_SIZE];
    uint32_t reboot;
    uint32_t dev_id;
    const char *proj;
    bool fw_match;

    if (!s_ready) {
        LOG_WARN("NVS: not initialized, skip boot info");
        return;
    }

    dev_id = nvs_device_id_get();
    proj = project_name_for_id(dev_id);
    fw_match = (dev_id == (uint32_t)NVS_DEFAULT_DEVICE_ID);

    LOG_INFO("--- NVS boot ---");
    LOG_INFO("  nvs_part: 0x%08lX size=%luKiB ns=%s", (unsigned long)NVS_FLASH_START_ADDR,
             (unsigned long)(NVS_FLASH_SIZE / 1024U), NVS_APP_NAMESPACE);
    if (boot_slot_format_status(boot_slot, sizeof(boot_slot)) == STATUS_OK) {
        LOG_INFO("  %s", boot_slot);
    }
    if (read_u32(NVS_KEY_REBOOT_COUNT, &reboot, 0)) {
        LOG_INFO("  reboot_count: %lu", (unsigned long)reboot);
    }
    if (nvs_sn_get(sn)) {
        LOG_INFO("  sn: %s", sn);
    }
    if (nvs_mac_get(mac)) {
        LOG_INFO("  mac: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 mac[6], mac[7]);
    }
    if (nvs_hw_version_get(hw)) {
        LOG_INFO("  hw: %s", hw);
    }
    {
        char app[NVS_APP_VERSION_SIZE];
        if (nvs_app_version_get(app)) {
            LOG_INFO("  app: %s (fw=%s%s)", app, NVS_APP_VERSION_STRING,
                     (strcmp(app, NVS_APP_VERSION_STRING) == 0) ? "" : " MISMATCH");
        }
    }
    {
        char factory[NVS_FACTORY_VERSION_SIZE];
        if (nvs_factory_version_get(factory)) {
            LOG_INFO("  factory: %s (fw=%s%s)", factory, NVS_FACTORY_VERSION_STRING,
                     (strcmp(factory, NVS_FACTORY_VERSION_STRING) == 0) ? "" : " MISMATCH");
        }
    }
    if (read_str(NVS_KEY_REGION, region, sizeof(region))) {
        LOG_INFO("  region: %s", region);
    }
    if (read_str(NVS_KEY_DEVICE_NAME, name, sizeof(name))) {
        LOG_INFO("  device_name: %s", name);
    }
    LOG_INFO("  device_type: %u", (unsigned int)nvs_device_type_get());
    if (dev_id == NVS_DEVICE_ID_DEFAULT) {
        LOG_INFO("  product_id: 0x%08lX (default)", (unsigned long)dev_id);
    } else if (proj != NULL) {
        LOG_INFO("  product_id: 0x%08lX (%s, fw=0x%08lX %s)", (unsigned long)dev_id, proj,
                 (unsigned long)(uint32_t)NVS_DEFAULT_DEVICE_ID, fw_match ? "match" : "MISMATCH");
    } else {
        LOG_INFO("  product_id: 0x%08lX (unknown, fw=0x%08lX %s)", (unsigned long)dev_id,
                 (unsigned long)(uint32_t)NVS_DEFAULT_DEVICE_ID, fw_match ? "match" : "MISMATCH");
    }
    {
        const uint32_t hw_id = nvs_hardware_id_get();
        LOG_INFO("  hardware_id: 0x%08lX", (unsigned long)hw_id);
    }
    {
        char bdate[NVS_BUILD_DATE_SIZE];
        uint32_t stored = nvs_build_date_get();
        uint32_t fw_bdate = nvs_firmware_build_date();

        if (nvs_build_date_format(stored, bdate, sizeof(bdate))) {
            LOG_INFO("  build_date: %s (fw=%08lu%s)", bdate, (unsigned long)fw_bdate,
                     (stored == fw_bdate) ? "" : " MISMATCH");
        }
    }
    {
        uint32_t run_sec;
        if (read_u32(NVS_KEY_RUN_TIME, &run_sec, 0) && (run_sec != 0U)) {
            LOG_INFO("  run_time: %lu sec", (unsigned long)run_sec);
        }
    }
    {
        char gal[NVS_LCD_GAL_BOOT_SIZE];
        if (nvs_lcd_gallery_boot_name_get(gal, sizeof(gal)) && (gal[0] != '\0')) {
            LOG_INFO("  lcd_gal_boot: %s", gal);
        }
    }
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

void nvs_init(void)
{
    esp_err_t err;
    uint32_t reboot;

    if (s_ready) {
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

    s_ready = true;
    ensure_defaults();

    if (read_u32(NVS_KEY_REBOOT_COUNT, &reboot, 0)) {
        reboot++;
    } else {
        reboot = 1U;
    }
    (void)write_u32(NVS_KEY_REBOOT_COUNT, reboot);

    LOG_INFO("NVS initialized");
    log_boot_info();
}

void nvs_factory_reset(void)
{
    esp_err_t err;

    if (!s_ready) {
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

    ensure_defaults();
    ESP_LOGI(TAG, "factory reset completed");
}

bool nvs_mac_set(const uint8_t *mac)
{
    if (!s_ready || (mac == NULL)) {
        return false;
    }
    return (blob_set(NVS_KEY_MAC, mac, NVS_MAC_SIZE) == NVS_OK);
}

bool nvs_mac_get(uint8_t *mac)
{
    size_t len = NVS_MAC_SIZE;

    if (!s_ready || (mac == NULL)) {
        return false;
    }
    return (blob_get(NVS_KEY_MAC, mac, &len) == NVS_OK);
}

bool nvs_sn_set(const char *sn)
{
    if (!s_ready || (sn == NULL)) {
        return false;
    }
    if (strlen(sn) >= NVS_SN_SIZE) {
        LOG_ERROR("NVS: SN too long");
        return false;
    }
    return (blob_set(NVS_KEY_SN, sn, strlen(sn) + 1U) == NVS_OK);
}

bool nvs_sn_get(char *sn)
{
    if (!read_str(NVS_KEY_SN, sn, NVS_SN_SIZE)) {
        return false;
    }
    return true;
}

bool nvs_hw_version_get(char *version)
{
    if (version == NULL) {
        return false;
    }
    copy_version(version, NVS_HW_VERSION_SIZE, NVS_HW_VERSION_STRING);
    return true;
}

bool nvs_app_version_get(char *version)
{
    if (version == NULL) {
        return false;
    }
    if (read_str(NVS_KEY_APP_VERSION, version, NVS_APP_VERSION_SIZE)) {
        return true;
    }
    copy_version(version, NVS_APP_VERSION_SIZE, NVS_APP_VERSION_STRING);
    return true;
}

bool nvs_factory_version_get(char *version)
{
    if (version == NULL) {
        return false;
    }
    if (read_str(NVS_KEY_FACTORY_VERSION, version, NVS_FACTORY_VERSION_SIZE)) {
        return true;
    }
    copy_version(version, NVS_FACTORY_VERSION_SIZE, NVS_FACTORY_VERSION_STRING);
    return true;
}

void nvs_device_type_set(uint8_t type)
{
    (void)write_u8(NVS_KEY_DEVICE_TYPE, type);
}

uint8_t nvs_device_type_get(void)
{
    uint8_t type = (uint8_t)NVS_DEFAULT_DEVICE_TYPE;
    (void)read_u8(NVS_KEY_DEVICE_TYPE, &type, (uint8_t)NVS_DEFAULT_DEVICE_TYPE);
    return type;
}

bool nvs_device_id_set(uint32_t id)
{
    if (!write_u32(NVS_KEY_DEVICE_ID, id)) {
        return false;
    }
    (void)device_name_apply(id);
    return true;
}

uint32_t nvs_device_id_get(void)
{
    uint32_t id = (uint32_t)NVS_DEFAULT_DEVICE_ID;
    (void)read_u32(NVS_KEY_DEVICE_ID, &id, (uint32_t)NVS_DEFAULT_DEVICE_ID);
    return id;
}

uint32_t nvs_product_id_active(void)
{
    const uint32_t stored = nvs_device_id_get();
    const uint32_t fw     = (uint32_t)NVS_DEFAULT_DEVICE_ID;

    if (fw != NVS_DEVICE_ID_DEFAULT) {
        return fw;
    }
    return stored;
}

bool nvs_hardware_id_set(uint32_t id)
{
    if (!write_u32(NVS_KEY_HARDWARE_ID, id)) {
        return false;
    }
    return true;
}

uint32_t nvs_hardware_id_get(void)
{
    uint32_t id = (uint32_t)NVS_DEFAULT_HARDWARE_ID;
    (void)read_u32(NVS_KEY_HARDWARE_ID, &id, (uint32_t)NVS_DEFAULT_HARDWARE_ID);
    return id;
}

const char *nvs_device_id_project_name(uint32_t id)
{
    return project_name_for_id(id);
}

uint32_t nvs_firmware_build_date(void)
{
    return (uint32_t)NVS_FIRMWARE_BUILD_DATE;
}

uint32_t nvs_build_date_get(void)
{
    uint32_t ymd = nvs_firmware_build_date();
    (void)read_u32(NVS_KEY_BUILD_DATE, &ymd, nvs_firmware_build_date());
    return ymd;
}

bool nvs_build_date_format(uint32_t ymd, char *out, size_t out_cap)
{
    if ((out == NULL) || (out_cap < NVS_BUILD_DATE_SIZE) || !build_date_valid(ymd)) {
        return false;
    }
    (void)snprintf(out, out_cap, "%08lu", (unsigned long)ymd);
    return true;
}

bool nvs_lcd_gallery_boot_name_set(const char *name)
{
    nvs_err_t st;

    if (!s_ready) {
        return false;
    }
    if ((name == NULL) || (name[0] == '\0')) {
        st = blob_del(NVS_KEY_LCD_GAL_BOOT);
        return (st == NVS_OK) || (st == NVS_ERR_NOT_FOUND);
    }
    if (strlen(name) >= NVS_LCD_GAL_BOOT_SIZE) {
        LOG_ERROR("NVS: lcd_gal_boot name too long");
        return false;
    }
    return (blob_set(NVS_KEY_LCD_GAL_BOOT, name, strlen(name) + 1U) == NVS_OK);
}

bool nvs_lcd_gallery_boot_name_get(char *out, size_t out_cap)
{
    if (!s_ready || (out == NULL) || (out_cap == 0U)) {
        return false;
    }
    if (!read_str(NVS_KEY_LCD_GAL_BOOT, out, out_cap)) {
        out[0] = '\0';
        return false;
    }
    return true;
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
_Static_assert(sizeof(nvs_web_ctrl_settings_t) <= NVS_KEY_VALUE_MAX, "web_ctrl blob must fit NVS_KEY_VALUE_MAX");

bool nvs_web_ctrl_settings_get(nvs_web_ctrl_settings_t *out)
{
    uint8_t buf[sizeof(nvs_web_ctrl_settings_t)];
    size_t len = sizeof(buf);

    if (!s_ready || (out == NULL)) {
        return false;
    }
    if (blob_get(NVS_KEY_WEB_CTRL, buf, &len) != NVS_OK) {
        return false;
    }
    if (len == sizeof(nvs_web_ctrl_settings_v1_t)) {
        nvs_web_ctrl_settings_v1_t v1;

        (void)memcpy(&v1, buf, sizeof(v1));
        (void)memset(out, 0, sizeof(*out));
        (void)memcpy(out, &v1, sizeof(v1));
        out->sta_ssid[0] = '\0';
        out->sta_password[0] = '\0';
    } else if (len == sizeof(nvs_web_ctrl_settings_t)) {
        (void)memcpy(out, buf, sizeof(*out));
    } else {
        return false;
    }
    return nvs_web_ctrl_settings_validate(out);
}

bool nvs_web_ctrl_settings_set(const nvs_web_ctrl_settings_t *cfg)
{
    if (!s_ready || (cfg == NULL)) {
        return false;
    }
    if (!nvs_web_ctrl_settings_validate(cfg)) {
        return false;
    }
    return (blob_set(NVS_KEY_WEB_CTRL, cfg, sizeof(*cfg)) == NVS_OK);
}

bool nvs_web_ctrl_settings_clear_sta_credentials(void)
{
    nvs_web_ctrl_settings_t st;

    if (!s_ready) {
        return false;
    }
    if (!nvs_web_ctrl_settings_get(&st)) {
        nvs_web_ctrl_settings_default(&st);
    }
    (void)memset(st.sta_ssid, 0, sizeof(st.sta_ssid));
    (void)memset(st.sta_password, 0, sizeof(st.sta_password));
    st.magic = NVS_WEB_CTRL_MAGIC;
    return nvs_web_ctrl_settings_set(&st);
}

bool nvs_web_ctrl_settings_delete(void)
{
    nvs_err_t st;

    if (!s_ready) {
        return false;
    }
    st = blob_del(NVS_KEY_WEB_CTRL);
    return (st == NVS_OK) || (st == NVS_ERR_NOT_FOUND);
}

void nvs_camera_settings_default(nvs_camera_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    (void)memset(out, 0, sizeof(*out));
    out->magic        = NVS_CAMERA_SETTINGS_MAGIC;
    out->web_width    = 240U;
    out->web_height   = 240U;
    out->quality      = 55U;
    out->grayscale    = 0U;
    out->zoom         = 1U;
    out->img_rotate   = 0U; /* 0° */
    out->flip_v       = 1U;
    out->flip_h       = 0U;
}

bool nvs_camera_settings_validate(const nvs_camera_settings_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->magic != NVS_CAMERA_SETTINGS_MAGIC) {
        return false;
    }
    if (!(((cfg->web_width == 240U) && (cfg->web_height == 240U)) ||
          ((cfg->web_width == 320U) && (cfg->web_height == 240U)) ||
          ((cfg->web_width == 640U) && (cfg->web_height == 480U)))) {
        return false;
    }
    if ((cfg->quality < 10U) || (cfg->quality > 95U)) {
        return false;
    }
    if (cfg->grayscale > 1U) {
        return false;
    }
    if ((cfg->zoom < 1U) || (cfg->zoom > 4U)) {
        return false;
    }
    if (cfg->img_rotate > 3U) {
        return false;
    }
    if ((cfg->flip_v > 1U) || (cfg->flip_h > 1U)) {
        return false;
    }
    return true;
}

_Static_assert(sizeof(nvs_camera_settings_t) <= NVS_KEY_VALUE_MAX, "camera settings blob must fit NVS_KEY_VALUE_MAX");

bool nvs_camera_settings_get(nvs_camera_settings_t *out)
{
    nvs_camera_settings_t tmp;
    size_t                len = sizeof(tmp);

    if (out == NULL) {
        return false;
    }
    if (blob_get(NVS_KEY_CAMERA_CFG, &tmp, &len) != NVS_OK) {
        return false;
    }
    if (len != sizeof(tmp) || !nvs_camera_settings_validate(&tmp)) {
        return false;
    }
    *out = tmp;
    return true;
}

bool nvs_camera_settings_set(const nvs_camera_settings_t *cfg)
{
    if (!s_ready || (cfg == NULL)) {
        return false;
    }
    if (!nvs_camera_settings_validate(cfg)) {
        return false;
    }
    return (blob_set(NVS_KEY_CAMERA_CFG, cfg, sizeof(*cfg)) == NVS_OK);
}

static void nvs_servo_pose_default(nvs_servo_pose_t *pose)
{
    if (pose == NULL) {
        return;
    }
    pose->angle_deg  = NVS_SERVO_CALIB_CENTER_DEG;
    pose->offset_deg = 0.0f;
    pose->pulse_us   = (uint16_t)NVS_SERVO_CALIB_CENTER_PULSE_US;
    pose->_pad       = 0U;
}

static bool nvs_servo_pose_validate(const nvs_servo_pose_t *pose)
{
    if (pose == NULL) {
        return false;
    }
    if ((pose->angle_deg < 0.0f) || (pose->angle_deg > NVS_SERVO_CALIB_ANGLE_MAX_DEG)) {
        return false;
    }
    if ((pose->offset_deg < -NVS_SERVO_CALIB_OFFSET_ABS_MAX) ||
        (pose->offset_deg > NVS_SERVO_CALIB_OFFSET_ABS_MAX)) {
        return false;
    }
    if ((pose->pulse_us < NVS_SERVO_CALIB_PULSE_MIN_US) || (pose->pulse_us > NVS_SERVO_CALIB_PULSE_MAX_US)) {
        return false;
    }
    return true;
}

void nvs_servo_calib_default(nvs_servo_calib_t *out)
{
    if (out == NULL) {
        return;
    }
    (void)memset(out, 0, sizeof(*out));
    out->magic       = NVS_SERVO_CALIB_MAGIC;
    out->channel     = NVS_SERVO_CALIB_CH_PAN; /* 平衡杠默认 Pan(GPIO46) */
    out->valid_mask  = 0U;
    nvs_servo_pose_default(&out->left);
    nvs_servo_pose_default(&out->center);
    nvs_servo_pose_default(&out->right);
}

bool nvs_servo_calib_validate(const nvs_servo_calib_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->magic != NVS_SERVO_CALIB_MAGIC) {
        return false;
    }
    if ((cfg->channel != NVS_SERVO_CALIB_CH_PAN) && (cfg->channel != NVS_SERVO_CALIB_CH_TILT)) {
        return false;
    }
    if ((cfg->valid_mask & (uint8_t)~NVS_SERVO_CALIB_VALID_ALL) != 0U) {
        return false;
    }
    if (!nvs_servo_pose_validate(&cfg->left) || !nvs_servo_pose_validate(&cfg->center) ||
        !nvs_servo_pose_validate(&cfg->right)) {
        return false;
    }
    return true;
}

_Static_assert(sizeof(nvs_servo_calib_t) <= NVS_KEY_VALUE_MAX, "servo calib blob must fit NVS_KEY_VALUE_MAX");

bool nvs_servo_calib_get(nvs_servo_calib_t *out)
{
    nvs_servo_calib_t tmp;
    size_t            len = sizeof(tmp);

    if (out == NULL) {
        return false;
    }
    if (blob_get(NVS_KEY_SERVO_CAL, &tmp, &len) != NVS_OK) {
        return false;
    }
    if (len != sizeof(tmp) || !nvs_servo_calib_validate(&tmp)) {
        return false;
    }
    *out = tmp;
    return true;
}

bool nvs_servo_calib_set(const nvs_servo_calib_t *cfg)
{
    if (!s_ready || (cfg == NULL)) {
        return false;
    }
    if (!nvs_servo_calib_validate(cfg)) {
        return false;
    }
    return (blob_set(NVS_KEY_SERVO_CAL, cfg, sizeof(*cfg)) == NVS_OK);
}

bool nvs_servo_calib_delete(void)
{
    nvs_err_t st;

    if (!s_ready) {
        return false;
    }
    st = blob_del(NVS_KEY_SERVO_CAL);
    return (st == NVS_OK) || (st == NVS_ERR_NOT_FOUND);
}

void nvs_touch_calib_default(nvs_touch_calib_t *out)
{
    if (out == NULL) {
        return;
    }
    (void)memset(out, 0, sizeof(*out));
    out->magic  = NVS_TOUCH_CALIB_MAGIC;
    out->valid  = 0U;
    out->ax_q16 = NVS_TOUCH_CALIB_Q16;
    out->ay_q16 = NVS_TOUCH_CALIB_Q16;
}

bool nvs_touch_calib_validate(const nvs_touch_calib_t *cfg)
{
    int32_t off_max_q16;

    if (cfg == NULL) {
        return false;
    }
    if (cfg->magic != NVS_TOUCH_CALIB_MAGIC) {
        return false;
    }
    if (cfg->valid > 1U) {
        return false;
    }
    if (cfg->valid == 0U) {
        return true;
    }
    if ((cfg->ax_q16 < NVS_TOUCH_CALIB_SCALE_MIN_Q16) || (cfg->ax_q16 > NVS_TOUCH_CALIB_SCALE_MAX_Q16) ||
        (cfg->ay_q16 < NVS_TOUCH_CALIB_SCALE_MIN_Q16) || (cfg->ay_q16 > NVS_TOUCH_CALIB_SCALE_MAX_Q16)) {
        return false;
    }
    off_max_q16 = (int32_t)NVS_TOUCH_CALIB_OFFSET_MAX_PX * NVS_TOUCH_CALIB_Q16;
    if ((cfg->bx_q16 < -off_max_q16) || (cfg->bx_q16 > off_max_q16) || (cfg->by_q16 < -off_max_q16) ||
        (cfg->by_q16 > off_max_q16)) {
        return false;
    }
    return true;
}

_Static_assert(sizeof(nvs_touch_calib_t) <= NVS_KEY_VALUE_MAX, "touch calib blob must fit NVS_KEY_VALUE_MAX");

bool nvs_touch_calib_get(nvs_touch_calib_t *out)
{
    nvs_touch_calib_t tmp;
    size_t            len = sizeof(tmp);

    if (out == NULL) {
        return false;
    }
    if (blob_get(NVS_KEY_TOUCH_CAL, &tmp, &len) != NVS_OK) {
        return false;
    }
    if (len != sizeof(tmp) || !nvs_touch_calib_validate(&tmp) || (tmp.valid == 0U)) {
        /* 清掉旧版过宽校验写下的坏数据，避免开机继续拉歪触摸 */
        (void)blob_del(NVS_KEY_TOUCH_CAL);
        return false;
    }
    *out = tmp;
    return true;
}

bool nvs_touch_calib_set(const nvs_touch_calib_t *cfg)
{
    if (!s_ready || (cfg == NULL)) {
        return false;
    }
    if (!nvs_touch_calib_validate(cfg) || (cfg->valid == 0U)) {
        return false;
    }
    return (blob_set(NVS_KEY_TOUCH_CAL, cfg, sizeof(*cfg)) == NVS_OK);
}

bool nvs_touch_calib_delete(void)
{
    nvs_err_t st;

    if (!s_ready) {
        return false;
    }
    st = blob_del(NVS_KEY_TOUCH_CAL);
    return (st == NVS_OK) || (st == NVS_ERR_NOT_FOUND);
}

void nvs_pet_needs_default(nvs_pet_needs_t *out)
{
    if (out == NULL) {
        return;
    }
    (void)memset(out, 0, sizeof(*out));
    out->magic = NVS_PET_NEEDS_MAGIC;
    out->hunger = 70U;
    out->mood = 70U;
    out->energy = 80U;
    out->sleeping = 0U;
}

bool nvs_pet_needs_validate(const nvs_pet_needs_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->magic != NVS_PET_NEEDS_MAGIC) {
        return false;
    }
    if ((cfg->hunger > 100U) || (cfg->mood > 100U) || (cfg->energy > 100U)) {
        return false;
    }
    if (cfg->sleeping > 1U) {
        return false;
    }
    return true;
}

_Static_assert(sizeof(nvs_pet_needs_t) <= NVS_KEY_VALUE_MAX, "pet needs blob must fit NVS_KEY_VALUE_MAX");

bool nvs_pet_needs_get(nvs_pet_needs_t *out)
{
    nvs_pet_needs_t tmp;
    size_t          len = sizeof(tmp);

    if (out == NULL) {
        return false;
    }
    if (blob_get(NVS_KEY_PET_NEEDS, &tmp, &len) != NVS_OK) {
        return false;
    }
    if ((len != sizeof(tmp)) || !nvs_pet_needs_validate(&tmp)) {
        (void)blob_del(NVS_KEY_PET_NEEDS);
        return false;
    }
    *out = tmp;
    return true;
}

bool nvs_pet_needs_set(const nvs_pet_needs_t *cfg)
{
    if (!s_ready || (cfg == NULL)) {
        return false;
    }
    if (!nvs_pet_needs_validate(cfg)) {
        return false;
    }
    return (blob_set(NVS_KEY_PET_NEEDS, cfg, sizeof(*cfg)) == NVS_OK);
}

bool nvs_pet_needs_delete(void)
{
    nvs_err_t st;

    if (!s_ready) {
        return false;
    }
    st = blob_del(NVS_KEY_PET_NEEDS);
    return (st == NVS_OK) || (st == NVS_ERR_NOT_FOUND);
}
