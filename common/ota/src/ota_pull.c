/**
 * @file ota_pull.c
 * @brief STA/HTTPS 云端 manifest 拉包，流式写入 `ota_upload_*` 状态机。
 */

#include "ota.h"

#include "sdkconfig.h"

#if CONFIG_OTA_HTTPS_PULL

#include "ota_manifest.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "ota_pull";

#ifndef CONFIG_OTA_HTTPS_PULL_CHUNK
#define OTA_HTTPS_PULL_CHUNK (4096U)
#else
#define OTA_HTTPS_PULL_CHUNK ((size_t)CONFIG_OTA_HTTPS_PULL_CHUNK)
#endif

#ifndef CONFIG_OTA_HTTPS_PULL_MANIFEST_MAX
#define OTA_HTTPS_PULL_MANIFEST_MAX (8192U)
#else
#define OTA_HTTPS_PULL_MANIFEST_MAX ((size_t)CONFIG_OTA_HTTPS_PULL_MANIFEST_MAX)
#endif

#ifndef CONFIG_OTA_HTTPS_PULL_TIMEOUT_MS
#define OTA_HTTPS_PULL_TIMEOUT_MS (60000)
#else
#define OTA_HTTPS_PULL_TIMEOUT_MS CONFIG_OTA_HTTPS_PULL_TIMEOUT_MS
#endif

#ifndef CONFIG_OTA_HTTPS_PULL_TASK_STACK
#define OTA_HTTPS_PULL_TASK_STACK (8192U)
#else
#define OTA_HTTPS_PULL_TASK_STACK CONFIG_OTA_HTTPS_PULL_TASK_STACK
#endif

#define OTA_HEADER_PEEK (4096U)

static TaskHandle_t     s_pull_task;
static volatile bool    s_pull_busy;
static ota_pull_request_t s_pull_req;

static status_t http_fetch_url(const char *url, uint8_t **out_body, size_t *out_len, int *out_status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = OTA_HTTPS_PULL_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = NULL;
    uint8_t                 *buf    = NULL;
    size_t                   cap    = 0U;
    size_t                   len    = 0U;
    status_t                 st     = ESP_OK;
    int                      status;
    int                      r;

    if ((url == NULL) || (out_body == NULL) || (out_len == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_body = NULL;
    *out_len  = 0U;
    if (out_status != NULL) {
        *out_status = 0;
    }

    client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (void)esp_http_client_set_method(client, HTTP_METHOD_GET);

    st = esp_http_client_open(client, 0);
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "open %s: %s", url, esp_err_to_name(st));
        esp_http_client_cleanup(client);
        return st;
    }

    r = esp_http_client_fetch_headers(client);
    if (r < 0) {
        ESP_LOGW(TAG, "GET %s fetch_headers failed", url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    status = esp_http_client_get_status_code(client);
    if (out_status != NULL) {
        *out_status = status;
    }
    if ((status < 200) || (status >= 300)) {
        ESP_LOGW(TAG, "GET %s HTTP %d", url, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_HTTP_BASE + status;
    }

    cap = OTA_HTTPS_PULL_MANIFEST_MAX;
    buf = (uint8_t *)malloc(cap);
    if (buf == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    while (true) {
        if (len >= cap) {
            st = ESP_ERR_NO_MEM;
            break;
        }
        r = esp_http_client_read(client, (char *)(buf + len), (int)(cap - len));
        if (r < 0) {
            st = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        len += (size_t)r;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (st != ESP_OK) {
        free(buf);
        return st;
    }

    *out_body = buf;
    *out_len  = len;
    return ESP_OK;
}

static status_t http_stream_image(const char *url, size_t expected_size, const ota_manifest_info_t *manifest)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = OTA_HTTPS_PULL_TIMEOUT_MS,
        .buffer_size = (int)OTA_HTTPS_PULL_CHUNK,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = NULL;
    uint8_t                 *chunk = NULL;
    uint8_t                  head[OTA_HEADER_PEEK];
    size_t                   got   = 0U;
    size_t                   total = 0U;
    status_t                 st;
    int                      status;
    int                      r;

    client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    chunk = (uint8_t *)malloc(OTA_HTTPS_PULL_CHUNK);
    if (chunk == NULL) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    (void)esp_http_client_set_method(client, HTTP_METHOD_GET);
    st = esp_http_client_open(client, 0);
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "image open: %s", esp_err_to_name(st));
        goto cleanup;
    }

    r = esp_http_client_fetch_headers(client);
    if (r < 0) {
        ESP_LOGW(TAG, "image GET fetch_headers failed");
        st = ESP_FAIL;
        goto cleanup;
    }

    status = esp_http_client_get_status_code(client);
    if ((status < 200) || (status >= 300)) {
        ESP_LOGW(TAG, "image GET HTTP %d", status);
        st = ESP_ERR_HTTP_BASE + status;
        goto cleanup;
    }

    total = (size_t)esp_http_client_get_content_length(client);
    if (total <= 0U) {
        if (manifest->image_size > 0U) {
            total = manifest->image_size;
        } else if (expected_size > 0U) {
            total = expected_size;
        } else {
            ESP_LOGW(TAG, "unknown Content-Length");
            st = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
    }
    if ((manifest->image_size > 0U) && (total != manifest->image_size)) {
        ESP_LOGW(TAG, "size mismatch manifest=%u http=%u", (unsigned)manifest->image_size, (unsigned)total);
        st = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    (void)ota_upload_abort();
    st = ota_upload_set_expected_sha256_hex(manifest->sha256_hex);
    if (st != ESP_OK) {
        goto cleanup;
    }

    while (got < OTA_HEADER_PEEK && got < total) {
        size_t want = total - got;

        if (want > OTA_HEADER_PEEK - got) {
            want = OTA_HEADER_PEEK - got;
        }
        r = esp_http_client_read(client, (char *)(head + got), (int)want);
        if (r < 0) {
            st = ESP_FAIL;
            goto cleanup;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    if (got < 256U) {
        st = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    st = ota_upload_begin(total, head, got);
    if (st != ESP_OK) {
        goto cleanup;
    }

    while (got < total) {
        size_t want = total - got;

        if (want > OTA_HTTPS_PULL_CHUNK) {
            want = OTA_HTTPS_PULL_CHUNK;
        }
        r = esp_http_client_read(client, (char *)chunk, (int)want);
        if (r < 0) {
            (void)ota_upload_abort();
            st = ESP_FAIL;
            goto cleanup;
        }
        if (r == 0) {
            (void)ota_upload_abort();
            st = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        st = ota_upload_write(chunk, (size_t)r);
        if (st != ESP_OK) {
            (void)ota_upload_abort();
            goto cleanup;
        }
        got += (size_t)r;
    }

    st = ota_upload_end();
    if (st != ESP_OK) {
        (void)ota_upload_abort();
    }

cleanup:
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(chunk);
    return st;
}

status_t ota_pull_from_manifest(const ota_pull_request_t *req)
{
    ota_manifest_info_t manifest;
    char                image_url[OTA_MANIFEST_URL_MAX];
    const esp_app_desc_t *run_desc;
    uint8_t            *manifest_body = NULL;
    size_t              manifest_len  = 0U;
    status_t            st;

    if ((req == NULL) || (req->manifest_url[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    st = ota_session_begin_cloud_pull();
    if (st != ESP_OK) {
        return st;
    }

    run_desc = esp_app_get_description();
    if (run_desc != NULL) {
        ESP_LOGI(TAG, "pull manifest=%s run_ver=%s", req->manifest_url, run_desc->version);
    }

    st = http_fetch_url(req->manifest_url, &manifest_body, &manifest_len, NULL);
    if (st != ESP_OK) {
        ota_session_end_cloud_pull();
        return st;
    }

    st = ota_manifest_parse((const char *)manifest_body, manifest_len,
                            (req->product[0] != '\0') ? req->product : NULL, &manifest);
    free(manifest_body);
    manifest_body = NULL;
    if (st != ESP_OK) {
        ota_session_end_cloud_pull();
        return st;
    }

    if ((run_desc != NULL) && !ota_version_is_greater(manifest.version, run_desc->version)) {
        ESP_LOGW(TAG, "manifest version %s not greater than %s", manifest.version, run_desc->version);
        ota_session_end_cloud_pull();
        return ESP_ERR_INVALID_VERSION;
    }

    st = ota_manifest_build_image_url(req->manifest_url, manifest.image_name, image_url, sizeof(image_url));
    if (st != ESP_OK) {
        ota_session_end_cloud_pull();
        return st;
    }

    ESP_LOGI(TAG, "pull image %s sha256=%.16s…", image_url, manifest.sha256_hex);

    st = http_stream_image(image_url, manifest.image_size, &manifest);
    ota_session_end_cloud_pull();
    if (st != ESP_OK) {
        return st;
    }

    if (req->apply_after_pull) {
        return ota_apply();
    }
    return ESP_OK;
}

static void ota_pull_task(void *arg)
{
    status_t st;

    (void)arg;
    st = ota_pull_from_manifest(&s_pull_req);
    if (st != ESP_OK) {
        ESP_LOGW(TAG, "pull failed: %s", esp_err_to_name(st));
    } else {
        ESP_LOGI(TAG, "pull complete");
    }
    s_pull_busy = false;
    s_pull_task = NULL;
    vTaskDelete(NULL);
}

status_t ota_pull_start(const ota_pull_request_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_pull_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pull_req = *req;
    s_pull_busy = true;

    if (xTaskCreate(ota_pull_task, "ota_pull", OTA_HTTPS_PULL_TASK_STACK, NULL, 5, &s_pull_task) != pdPASS) {
        s_pull_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else /* CONFIG_OTA_HTTPS_PULL */

status_t ota_pull_from_manifest(const ota_pull_request_t *req)
{
    (void)req;
    return ESP_ERR_NOT_SUPPORTED;
}

status_t ota_pull_start(const ota_pull_request_t *req)
{
    (void)req;
    return ESP_ERR_NOT_SUPPORTED;
}

status_t ota_session_begin_cloud_pull(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ota_session_end_cloud_pull(void)
{
}

#endif /* CONFIG_OTA_HTTPS_PULL */
