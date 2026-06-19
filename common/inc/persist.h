/**
 * @file    persist.h
 * @brief   非易失参数层（ESP32-S3 / ESP-IDF NVS），API 对齐 STM32 工程 `tmp/nvs.h` 习惯用法。
 *
 * ESP-IDF 自带 `nvs.h`，本模块单字文件名 `persist.h` 避免与官方头同名冲突；对外函数仍为 `nvs_init`、`nvs_set` 等。
 *
 * 数据在命名空间 `NVS_APP_NAMESPACE` 下以 blob 存储，与 STM32 侧 raw 字节语义一致。
 */

#ifndef COMMON_PERSIST_H
#define COMMON_PERSIST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 与 ESP-IDF `NVS_KEY_NAME_MAX_SIZE` 一致：含结尾 NUL 共 16，键名至多 15 字符。 */
#define NVS_KEY_MAX_LEN 16
/** 单键 blob 最大长度（须 ≥ 最大 NVS 结构体，如 `nvs_web_ctrl_settings_t`）。 */
#define NVS_VALUE_MAX_LEN 256

#define NVS_MAC_SIZE 8
#define NVS_SN_SIZE 16
#define NVS_HW_VERSION_SIZE 16
#define NVS_APP_VERSION_SIZE 24
#define NVS_FACTORY_VERSION_SIZE 24
#define NVS_REGION_SIZE 8
/** SD 图库开机默认图：`picture/` 下文件名（如 `W01A2B3C.BMP`），与 `lcd_gallery` 扫描名一致。 */
#define NVS_LCD_GAL_BOOT_SIZE 121

#define NVS_DEFAULT_MAC                                                                                              \
    { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 }
#define NVS_DEFAULT_SN "T90M0123456789"
#define NVS_DEFAULT_REGION "US915"

#define NVS_HW_VERSION_STRING "1.0.0"
#define NVS_APP_VERSION_STRING "1.0.0"
#define NVS_FACTORY_VERSION_STRING "1.0.0"
#define NVS_DEFAULT_DEVICE_TYPE 1
#define NVS_DEFAULT_RUN_TIME 0U
#define NVS_DEFAULT_LAST_HOURLY_RECORD 0U

/** NVS 分区中本应用使用的命名空间（可按产品修改）。 */
#define NVS_APP_NAMESPACE "ty_app"

typedef enum {
    NVS_OK = 0,
    NVS_ERROR_INVALID_PARAM,
    NVS_ERROR_NOT_INITIALIZED,
    NVS_ERROR_NOT_FOUND,
    NVS_ERROR_NO_SPACE,
    NVS_ERROR_FLASH_ERASE,
    NVS_ERROR_FLASH_WRITE,
    NVS_ERROR_INVALID_STATE,
    NVS_ERROR_BUSY,
    NVS_ERROR_CRC
} nvs_status_t;

/**
 * 与 STM32 版 `nvs_handle_t` 对应的占位上下文：ESP-IDF 在 `persist.c` 内持有打开的 `nvs_handle_t`，
 * 对外仅保留初始化标志供 `nvs_set` 等做前置检查。
 */
typedef struct {
    bool initialized;
} nvs_ctx_t;

nvs_status_t nvs_set(nvs_ctx_t *nvs, const char *key, const void *value, size_t value_len);
nvs_status_t nvs_get(nvs_ctx_t *nvs, const char *key, void *value, size_t *value_len);
nvs_status_t nvs_delete(nvs_ctx_t *nvs, const char *key);

extern nvs_ctx_t g_nvs_handle;

void nvs_init(void);
void nvs_factory_reset(void);

void nvs_reboot_count_set(uint32_t count);
uint32_t nvs_reboot_count_get(void);
void nvs_frame_count_set(uint32_t count);
uint32_t nvs_frame_count_get(void);
void nvs_heartbeat_seq_set(uint32_t seq);
uint32_t nvs_heartbeat_seq_get(void);

bool nvs_mac_set(const uint8_t *mac);
bool nvs_mac_get(uint8_t *mac);
bool nvs_sn_set(const char *sn);
bool nvs_sn_get(char *sn);
bool nvs_hw_version_get(char *version);
bool nvs_app_version_get(char *version);
bool nvs_factory_version_get(char *version);
bool nvs_region_set(const char *region);
bool nvs_region_get(char *region);
void nvs_device_type_set(uint8_t type);
uint8_t nvs_device_type_get(void);

void nvs_run_time_set(uint32_t sec);
uint32_t nvs_run_time_get(void);
void nvs_last_hourly_record_set(uint32_t sec);
uint32_t nvs_last_hourly_record_get(void);

bool nvs_buzzer_duration_sec_set(uint32_t sec);
bool nvs_buzzer_duration_sec_get(uint32_t *out);

#define NVS_ATT_LEVEL_MAGIC 0x314C5441u /* "ATL1" LE */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    int32_t roll_mdeg;
    int32_t pitch_mdeg;
} nvs_attitude_level_cal_t;

bool nvs_attitude_level_cal_set(const nvs_attitude_level_cal_t *cal);
bool nvs_attitude_level_cal_get(nvs_attitude_level_cal_t *out);
bool nvs_attitude_level_cal_delete(void);

#define NVS_WEB_CTRL_MAGIC 0x57434231u /* "WCB1" LE */

/** NVS 中保存的 Web 控制（SoftAP + HTTP 端口 + 可选 STA）参数；与 `net_wifi_config_t` / `web_ctrl_config_t` 字段对齐。 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    char softap_ssid[33];
    char softap_password[65];
    uint8_t softap_channel;
    uint8_t softap_max_connection;
    uint16_t http_port;
    uint8_t reserved[2];
    /** 目标路由器 SSID；全 `\\0` 表示不在 NVS 中配置 STA（上电仅 SoftAP 配网）。 */
    char sta_ssid[33];
    /** STA 密码；开放网络可为空。 */
    char sta_password[65];
} nvs_web_ctrl_settings_t;

void nvs_web_ctrl_settings_default(nvs_web_ctrl_settings_t *out);
bool nvs_web_ctrl_settings_validate(const nvs_web_ctrl_settings_t *cfg);
bool nvs_web_ctrl_settings_get(nvs_web_ctrl_settings_t *out);
bool nvs_web_ctrl_settings_set(const nvs_web_ctrl_settings_t *cfg);
/** 清空 NVS 中 STA 路由器 SSID/密码，保留 SoftAP/端口等其余字段；写入失败返回 false。 */
bool nvs_web_ctrl_settings_clear_sta_credentials(void);
bool nvs_web_ctrl_settings_delete(void);

/** 写入开机默认图文件名（仅 basename）；`name` 为空或 `NULL` 表示清除。 */
bool nvs_lcd_gallery_boot_name_set(const char *name);
/** 读取已存的开机默认图文件名；未配置返回 `false` 且 `out[0]` 为 NUL。 */
bool nvs_lcd_gallery_boot_name_get(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_PERSIST_H */
