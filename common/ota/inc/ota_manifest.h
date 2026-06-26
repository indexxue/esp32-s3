/**
 * @file ota_manifest.h
 * @brief Release manifest.json（schema 2）轻量解析，见 firmware/README.md。
 */

#ifndef COMMON_OTA_MANIFEST_H
#define COMMON_OTA_MANIFEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "type.h"

#include <stddef.h>

#define OTA_MANIFEST_VERSION_MAX   (32U)
#define OTA_MANIFEST_PRODUCT_MAX   (32U)
#define OTA_MANIFEST_IMAGE_MAX     (96U)
#define OTA_MANIFEST_SHA256_HEX_MAX (65U)
#define OTA_MANIFEST_URL_MAX       (512U)

typedef struct {
    char    version[OTA_MANIFEST_VERSION_MAX];
    char    product[OTA_MANIFEST_PRODUCT_MAX];
    char    image_name[OTA_MANIFEST_IMAGE_MAX];
    char    sha256_hex[OTA_MANIFEST_SHA256_HEX_MAX];
    size_t  image_size;
} ota_manifest_info_t;

/**
 * @brief 从 manifest JSON 提取指定 product 的 OTA 镜像信息。
 * @param product  如 "project"；NULL 时使用 "project"。
 */
status_t ota_manifest_parse(const char *json, size_t json_len, const char *product, ota_manifest_info_t *out);

/**
 * @brief 将 manifest URL 与相对镜像名拼成绝对 image URL。
 * @param manifest_url  manifest.json 完整 URL。
 * @param image_name    manifest 中 ota.image 字段。
 */
status_t ota_manifest_build_image_url(const char *manifest_url, const char *image_name, char *out_url,
                                      size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_OTA_MANIFEST_H */
