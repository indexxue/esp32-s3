/**
 * @file    nvs.h
 * @brief   应用层非易失参数 API（ESP32-S3 / ESP-IDF NVS）。
 *
 * 数据存储在命名空间 NVS_APP_NAMESPACE 下，以 blob 形式读写。
 * 片内 NVS 分区见 flash_partition.h（128KiB @ 0x9000）。
 *
 * 新增项目：在 NVS_PROJECT_ID_TABLE 登记，并同步 common/cmake/nvs_project_id.cmake。
 */

#ifndef COMMON_NVS_H
#define COMMON_NVS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash_partition.h"

/* -------------------------------------------------------------------------- */
/* 项目 product_id 登记表（NVS 键 did；factory 使用默认 ID 0）                  */
/* hardware_id 见 device_profile.h / NVS 键 hid                                 */
/* -------------------------------------------------------------------------- */

#define NVS_DEVICE_ID_DEFAULT 0U

/** 项目 ID 编码：0x5353PPNN（PP=类别 0x01，NN=项目序号） */
#define NVS_PROJECT_ID_CAT_PROJECT 0x53530100U
#define NVS_PROJECT_ID_MAKE(index) (NVS_PROJECT_ID_CAT_PROJECT | ((uint32_t)(index) & 0xFFU))

#define NVS_PROJECT_ID_MAIN NVS_PROJECT_ID_MAKE(0x01U)
#define NVS_PROJECT_ID_BALLOT_GUARD NVS_PROJECT_ID_MAKE(0x02U)

#define NVS_PROJECT_ID_TABLE(X)                                                                                      \
    X(MAIN, NVS_PROJECT_ID_MAIN, "main")                                                                             \
    X(BALLOT_GUARD, NVS_PROJECT_ID_BALLOT_GUARD, "ballot_guard")

#define NVS_DEVICE_ID_IS_PROJECT(id) (((uint32_t)(id) & 0xFFFFFF00U) == NVS_PROJECT_ID_CAT_PROJECT)

/** 硬件型号 ID（NVS 键 hid）：0x5353HHNN（HH=类别 0x02） */
#define NVS_HARDWARE_ID_CAT 0x53530200U
#define NVS_HARDWARE_ID_MAKE(index) (NVS_HARDWARE_ID_CAT | ((uint32_t)(index) & 0xFFU))
#define NVS_HARDWARE_ID_TY_S3_REV_A NVS_HARDWARE_ID_MAKE(0x01U)

#ifndef NVS_DEFAULT_HARDWARE_ID
#define NVS_DEFAULT_HARDWARE_ID NVS_HARDWARE_ID_TY_S3_REV_A
#endif

#ifndef NVS_DEFAULT_DEVICE_ID
#define NVS_DEFAULT_DEVICE_ID NVS_DEVICE_ID_DEFAULT
#endif

/* -------------------------------------------------------------------------- */
/* 容量与默认值                                                                */
/* -------------------------------------------------------------------------- */

#define NVS_MAC_SIZE 8
#define NVS_SN_SIZE 16
#define NVS_HW_VERSION_SIZE 16
#define NVS_APP_VERSION_SIZE 24
#define NVS_FACTORY_VERSION_SIZE 24
#define NVS_LCD_GAL_BOOT_SIZE 121

#define NVS_DEFAULT_MAC                                                                                              \
    { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 }
#define NVS_DEFAULT_SN "T90M0123456789"
#define NVS_DEFAULT_REGION "US915"
#define NVS_DEVICE_NAME_DEFAULT "factory"

#define NVS_HW_VERSION_STRING "1.0.0"
/** 应用固件版本，由 common/CMakeLists.txt 从 PROJECT_VER 注入；未注入时见默认值。 */
#ifndef NVS_APP_VERSION_STRING
#define NVS_APP_VERSION_STRING "1.0.0"
#endif
/** 工厂固件版本，由 common/CMakeLists.txt 从 PROJECT_VER 注入。 */
#ifndef NVS_FACTORY_VERSION_STRING
#define NVS_FACTORY_VERSION_STRING "1.0.0"
#endif
#define NVS_DEFAULT_DEVICE_TYPE 1

#define NVS_FLASH_START_ADDR FLASH_PART_NVS_START
#define NVS_FLASH_SIZE FLASH_PART_NVS_SIZE
#define NVS_APP_NAMESPACE "ty_app"

/** 固件构建日期格式：YYYYMMDD，例如 20260620 */
#define NVS_BUILD_DATE_SIZE 9

/* -------------------------------------------------------------------------- */
/* 生命周期                                                                    */
/* -------------------------------------------------------------------------- */

void nvs_init(void);
void nvs_factory_reset(void);

/* -------------------------------------------------------------------------- */
/* 设备标识                                                                    */
/* -------------------------------------------------------------------------- */

bool nvs_mac_set(const uint8_t *mac);
bool nvs_mac_get(uint8_t *mac);
bool nvs_sn_set(const char *sn);
bool nvs_sn_get(char *sn);
bool nvs_hw_version_get(char *version);
bool nvs_app_version_get(char *version);
bool nvs_factory_version_get(char *version);
void nvs_device_type_set(uint8_t type);
uint8_t nvs_device_type_get(void);

bool nvs_device_id_set(uint32_t id);
uint32_t nvs_device_id_get(void);
/** 与 nvs_device_id_get/set 相同，语义为软件产品 ID（NVS 键 did）。 */
#define nvs_product_id_get nvs_device_id_get
#define nvs_product_id_set nvs_device_id_set

bool nvs_hardware_id_set(uint32_t id);
uint32_t nvs_hardware_id_get(void);

/**
 * 运行时生效的 product_id：项目固件用编译期 NVS_DEFAULT_DEVICE_ID；factory 固件读 NVS。
 */
uint32_t nvs_product_id_active(void);
/** @deprecated 使用 nvs_product_id_active */
#define nvs_device_id_for_init nvs_product_id_active

const char *nvs_device_id_project_name(uint32_t id);
#define nvs_product_id_name nvs_device_id_project_name

uint32_t nvs_firmware_build_date(void);
uint32_t nvs_build_date_get(void);
bool nvs_build_date_format(uint32_t ymd, char *out, size_t out_cap);

/* -------------------------------------------------------------------------- */
/* Web 控制 / Wi-Fi 配置                                                       */
/* -------------------------------------------------------------------------- */

#define NVS_WEB_CTRL_MAGIC 0x57434231u /* "WCB1" LE */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    char softap_ssid[33];
    char softap_password[65];
    uint8_t softap_channel;
    uint8_t softap_max_connection;
    uint16_t http_port;
    uint8_t reserved[2];
    char sta_ssid[33];
    char sta_password[65];
} nvs_web_ctrl_settings_t;

void nvs_web_ctrl_settings_default(nvs_web_ctrl_settings_t *out);
bool nvs_web_ctrl_settings_validate(const nvs_web_ctrl_settings_t *cfg);
bool nvs_web_ctrl_settings_get(nvs_web_ctrl_settings_t *out);
bool nvs_web_ctrl_settings_set(const nvs_web_ctrl_settings_t *cfg);
bool nvs_web_ctrl_settings_clear_sta_credentials(void);
bool nvs_web_ctrl_settings_delete(void);

/* -------------------------------------------------------------------------- */
/* LCD 图库启动项                                                              */
/* -------------------------------------------------------------------------- */

bool nvs_lcd_gallery_boot_name_set(const char *name);
bool nvs_lcd_gallery_boot_name_get(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_NVS_H */
