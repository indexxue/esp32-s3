/**
 * @file ota_manifest.c
 * @brief schema 2 manifest 轻量 JSON 解析（无第三方 JSON 库）。
 */

#include "ota_manifest.h"

#include "esp_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ota_manifest";

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

static bool json_find_object_value(const char *json, size_t json_len, const char *key, const char **out_start,
                                   const char **out_end)
{
    char     needle[64];
    size_t   key_len;
    const char *found;
    const char *p;
    const char *end;
    int        depth;

    if ((json == NULL) || (key == NULL) || (out_start == NULL) || (out_end == NULL)) {
        return false;
    }
    end = json + json_len;
    key_len = strlen(key);
    if (key_len + 4U >= sizeof(needle)) {
        return false;
    }
    (void)snprintf(needle, sizeof(needle), "\"%s\"", key);
    found = json;
    while ((found != NULL) && (found < end)) {
        found = strstr(found, needle);
        if (found == NULL) {
            return false;
        }
        if ((found > json) && (isalnum((unsigned char)found[-1]) != 0)) {
            found += strlen(needle);
            continue;
        }
        p = found + strlen(needle);
        if (!json_skip_ws(&p) || (p >= end) || (*p != ':')) {
            found += strlen(needle);
            continue;
        }
        p++;
        if (!json_skip_ws(&p) || (p >= end)) {
            return false;
        }
        if (*p == '"') {
            *out_start = p + 1U;
            p++;
            while ((p < end) && (*p != '"')) {
                if ((*p == '\\') && ((p + 1U) < end)) {
                    p++;
                }
                p++;
            }
            if ((p >= end) || (*p != '"')) {
                return false;
            }
            *out_end = p;
            return true;
        }
        if (*p != '{') {
            return false;
        }
        *out_start = p;
        depth = 0;
        while (p < end) {
            if (*p == '{') {
                depth++;
            } else if (*p == '}') {
                depth--;
                if (depth == 0) {
                    *out_end = p + 1U;
                    return true;
                }
            } else if (*p == '"') {
                p++;
                while ((p < end) && (*p != '"')) {
                    if ((*p == '\\') && ((p + 1U) < end)) {
                        p++;
                    }
                    p++;
                }
            }
            p++;
        }
        return false;
    }
    return false;
}

static bool json_copy_quoted(const char *start, const char *end, char *out, size_t out_cap)
{
    size_t o = 0U;
    const char *p;

    if ((start == NULL) || (end == NULL) || (out_cap == 0U)) {
        return false;
    }
    for (p = start; (p < end) && (*p != '"'); p++) {
        if ((*p == '\\') && ((p + 1U) < end)) {
            p++;
        }
        if (o + 1U >= out_cap) {
            return false;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return true;
}

static bool json_extract_uint64_field(const char *block, size_t block_len, const char *key, uint64_t *out)
{
    char     needle[32];
    const char *found;
    const char *p;
    const char *end;
    char    *tail;

    if ((block == NULL) || (key == NULL) || (out == NULL)) {
        return false;
    }
    end = block + block_len;
    (void)snprintf(needle, sizeof(needle), "\"%s\"", key);
    found = block;
    while ((found != NULL) && (found < end)) {
        found = strstr(found, needle);
        if (found == NULL) {
            return false;
        }
        p = found + strlen(needle);
        if (!json_skip_ws(&p) || (p >= end) || (*p != ':')) {
            found += strlen(needle);
            continue;
        }
        p++;
        if (!json_skip_ws(&p) || (p >= end)) {
            return false;
        }
        *out = strtoull(p, &tail, 10);
        return (tail > p);
    }
    return false;
}

static bool json_extract_quoted_in_block(const char *block, size_t block_len, const char *key, char *out,
                                         size_t out_cap)
{
    const char *val_start;
    const char *val_end;

    if (!json_find_object_value(block, block_len, key, &val_start, &val_end)) {
        return false;
    }
    return json_copy_quoted(val_start, val_end, out, out_cap);
}

static bool ota_manifest_sha256_valid(const char *hex)
{
    size_t i;

    if (hex == NULL) {
        return false;
    }
    if (strlen(hex) != 64U) {
        return false;
    }
    for (i = 0U; hex[i] != '\0'; i++) {
        if (isxdigit((unsigned char)hex[i]) == 0) {
            return false;
        }
    }
    return true;
}

static bool ota_manifest_find_app_bin_sha256(const char *product_block, size_t product_len, const char *image_name,
                                             char *sha256_out, size_t sha256_cap, size_t *size_out)
{
    const char *artifacts;
    const char *p;
    const char *end;
    char        file[OTA_MANIFEST_IMAGE_MAX];
    char        role[16];
    char        format[32];
    char        sha256[OTA_MANIFEST_SHA256_HEX_MAX];
    uint64_t    size_val;

    artifacts = strstr(product_block, "\"artifacts\"");
    if (artifacts == NULL) {
        return false;
    }
    end = product_block + product_len;
    p   = artifacts;
    while ((p != NULL) && (p < end)) {
        p = strstr(p, "\"file\"");
        if ((p == NULL) || (p >= end)) {
            break;
        }
        if (!json_extract_quoted_in_block(p, (size_t)(end - p), "file", file, sizeof(file))) {
            p += 6U;
            continue;
        }
        if (strcmp(file, image_name) != 0) {
            p += 6U;
            continue;
        }
        role[0]   = '\0';
        format[0] = '\0';
        (void)json_extract_quoted_in_block(p, (size_t)(end - p), "role", role, sizeof(role));
        (void)json_extract_quoted_in_block(p, (size_t)(end - p), "format", format, sizeof(format));
        if ((role[0] != '\0') && (strcmp(role, "app") != 0)) {
            p += 6U;
            continue;
        }
        if ((format[0] != '\0') && (strcmp(format, "intel_hex") == 0)) {
            p += 6U;
            continue;
        }
        sha256[0] = '\0';
        if (!json_extract_quoted_in_block(p, (size_t)(end - p), "sha256", sha256, sizeof(sha256))) {
            return false;
        }
        if (!ota_manifest_sha256_valid(sha256)) {
            return false;
        }
        (void)snprintf(sha256_out, sha256_cap, "%s", sha256);
        if (size_out != NULL) {
            if (json_extract_uint64_field(p, (size_t)(end - p), "size", &size_val)) {
                *size_out = (size_t)size_val;
            } else {
                *size_out = 0U;
            }
        }
        return true;
    }
    return false;
}

status_t ota_manifest_parse(const char *json, size_t json_len, const char *product, ota_manifest_info_t *out)
{
    const char *products_start;
    const char *products_end;
    const char *product_start;
    const char *product_end;
    const char *prod;
    char        product_key[OTA_MANIFEST_PRODUCT_MAX];
    char        version[OTA_MANIFEST_VERSION_MAX];
    char        image[OTA_MANIFEST_IMAGE_MAX];
    char        sha256[OTA_MANIFEST_SHA256_HEX_MAX];
    size_t      image_size = 0U;
    const char *ota_start  = NULL;
    const char *ota_end    = NULL;

    if ((json == NULL) || (json_len == 0U) || (out == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    prod = (product != NULL && product[0] != '\0') ? product : "project";
    (void)memset(out, 0, sizeof(*out));
    sha256[0] = '\0';
    image[0]  = '\0';

    if (!json_find_object_value(json, json_len, "version", &products_start, &products_end)) {
        ESP_LOGW(TAG, "manifest missing version");
        return ESP_ERR_NOT_FOUND;
    }
    if (!json_copy_quoted(products_start, products_end, version, sizeof(version))) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!json_find_object_value(json, json_len, "products", &products_start, &products_end)) {
        ESP_LOGW(TAG, "manifest missing products");
        return ESP_ERR_NOT_FOUND;
    }

    (void)snprintf(product_key, sizeof(product_key), "%s", prod);
    if (!json_find_object_value(products_start, (size_t)(products_end - products_start), product_key, &product_start,
                                 &product_end)) {
        ESP_LOGW(TAG, "product %s not in manifest", prod);
        return ESP_ERR_NOT_FOUND;
    }

    if (!json_find_object_value(product_start, (size_t)(product_end - product_start), "ota", &ota_start, &ota_end)) {
        ESP_LOGW(TAG, "product %s missing ota", prod);
        return ESP_ERR_NOT_FOUND;
    }
    if (!json_extract_quoted_in_block(ota_start, (size_t)(ota_end - ota_start), "image", image, sizeof(image))) {
        ESP_LOGW(TAG, "product %s missing ota.image", prod);
        return ESP_ERR_NOT_FOUND;
    }

    if (!ota_manifest_find_app_bin_sha256(product_start, (size_t)(product_end - product_start), image, sha256,
                                          sizeof(sha256), &image_size)) {
        (void)json_extract_quoted_in_block(ota_start, (size_t)(ota_end - ota_start), "sha256", sha256, sizeof(sha256));
        if (!ota_manifest_sha256_valid(sha256)) {
            ESP_LOGW(TAG, "missing or invalid sha256 for %s", image);
            return ESP_ERR_INVALID_CRC;
        }
    }

    (void)snprintf(out->version, sizeof(out->version), "%s", version);
    (void)snprintf(out->product, sizeof(out->product), "%s", prod);
    (void)snprintf(out->image_name, sizeof(out->image_name), "%s", image);
    (void)snprintf(out->sha256_hex, sizeof(out->sha256_hex), "%s", sha256);
    out->image_size = image_size;
    return ESP_OK;
}

status_t ota_manifest_build_image_url(const char *manifest_url, const char *image_name, char *out_url, size_t out_cap)
{
    const char *slash;
    size_t      base_len;

    if ((manifest_url == NULL) || (image_name == NULL) || (out_url == NULL) || (out_cap == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((strstr(manifest_url, "://") != NULL) && (strchr(image_name, '/') != NULL)) {
        (void)snprintf(out_url, out_cap, "%s", image_name);
        return ESP_OK;
    }

    slash = strrchr(manifest_url, '/');
    if (slash == NULL) {
        (void)snprintf(out_url, out_cap, "%s/%s", manifest_url, image_name);
        return ESP_OK;
    }
    base_len = (size_t)(slash - manifest_url) + 1U;
    if (base_len + strlen(image_name) + 1U >= out_cap) {
        return ESP_ERR_NO_MEM;
    }
    (void)memcpy(out_url, manifest_url, base_len);
    out_url[base_len] = '\0';
    (void)strcat(out_url, image_name);
    return ESP_OK;
}
