/**
 * @file ota.c
 * @brief 对侧槽 OTA 会话：版本比较、写入、apply/abort。
 */

#include "ota.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mbedtls/sha256.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ota";

#define OTA_HEADER_PEEK_MIN (256U)

static SemaphoreHandle_t s_ota_mtx;
static esp_ota_handle_t  s_ota_handle;
static const esp_partition_t *s_target;
static size_t              s_expected;
static size_t              s_written;
static ota_session_state_e s_state;
static char                s_pending_version[sizeof(((esp_app_desc_t *)0)->version)];
static mbedtls_sha256_context s_sha256;
static bool                s_sha256_active;
static uint8_t             s_expected_sha256[32];
static bool                s_expect_sha256;
static bool                s_cloud_pull;

static status_t ota_lock(void)
{
    if (s_ota_mtx == NULL) {
        s_ota_mtx = xSemaphoreCreateMutex();
        if (s_ota_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_ota_mtx, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void ota_unlock(void)
{
    (void)xSemaphoreGive(s_ota_mtx);
}

static void ota_reset_session(void)
{
    s_ota_handle = 0;
    s_target     = NULL;
    s_expected   = 0U;
    s_written    = 0U;
    s_state      = OTA_SESSION_IDLE;
    s_pending_version[0] = '\0';
    s_sha256_active      = false;
    s_expect_sha256      = false;
    s_cloud_pull         = false;
}

static bool ota_find_app_desc(const uint8_t *data, size_t len, esp_app_desc_t *out)
{
    size_t i;

    if ((data == NULL) || (out == NULL) || (len < sizeof(esp_app_desc_t))) {
        return false;
    }
    for (i = 0U; i + sizeof(esp_app_desc_t) <= len; i++) {
        const esp_app_desc_t *d = (const esp_app_desc_t *)(const void *)(data + i);

        if (d->magic_word == ESP_APP_DESC_MAGIC_WORD) {
            *out = *d;
            return true;
        }
    }
    return false;
}

static bool ota_parse_version_triplet(const char *ver, int *maj, int *min, int *pat)
{
    unsigned um = 0U;
    unsigned ui = 0U;
    unsigned up = 0U;
    int      n;

    if (ver == NULL) {
        return false;
    }
    n = sscanf(ver, "%u.%u.%u", &um, &ui, &up);
    if (n < 2) {
        return false;
    }
    *maj = (int)um;
    *min = (int)ui;
    *pat = (n >= 3) ? (int)up : 0;
    return true;
}

bool ota_version_is_greater(const char *new_ver, const char *cur_ver)
{
    int nma = 0;
    int nmi = 0;
    int npa = 0;
    int cma = 0;
    int cmi = 0;
    int cpa = 0;

    if (!ota_parse_version_triplet(new_ver, &nma, &nmi, &npa)) {
        return false;
    }
    if (!ota_parse_version_triplet(cur_ver, &cma, &cmi, &cpa)) {
        return true;
    }
    if (nma != cma) {
        return nma > cma;
    }
    if (nmi != cmi) {
        return nmi > cmi;
    }
    return npa > cpa;
}

static bool ota_hex_nibble(char c, uint8_t *out)
{
    if ((c >= '0') && (c <= '9')) {
        *out = (uint8_t)(c - '0');
        return true;
    }
    if ((c >= 'a') && (c <= 'f')) {
        *out = (uint8_t)(c - 'a' + 10);
        return true;
    }
    if ((c >= 'A') && (c <= 'F')) {
        *out = (uint8_t)(c - 'A' + 10);
        return true;
    }
    return false;
}

static bool ota_parse_sha256_hex(const char *hex, uint8_t out[32])
{
    size_t i;

    if (hex == NULL) {
        return false;
    }
    if (strlen(hex) != 64U) {
        return false;
    }
    for (i = 0U; i < 32U; i++) {
        uint8_t hi;
        uint8_t lo;

        if (!ota_hex_nibble(hex[i * 2U], &hi) || !ota_hex_nibble(hex[(i * 2U) + 1U], &lo)) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void ota_sha256_update(const uint8_t *data, size_t len)
{
    if (!s_sha256_active || (data == NULL) || (len == 0U)) {
        return;
    }
    (void)mbedtls_sha256_update(&s_sha256, data, len);
}

static status_t ota_sha256_verify(void)
{
    uint8_t digest[32];

    if (!s_expect_sha256) {
        return ESP_OK;
    }
    if (!s_sha256_active) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)mbedtls_sha256_finish(&s_sha256, digest);
    s_sha256_active = false;
    if (memcmp(digest, s_expected_sha256, sizeof(digest)) != 0) {
        ESP_LOGW(TAG, "SHA256 mismatch");
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "SHA256 verified");
    return ESP_OK;
}

static void ota_copy_label(char *dst, size_t cap, const char *label)
{
    size_t i;

    if ((dst == NULL) || (cap == 0U)) {
        return;
    }
    dst[0] = '\0';
    if (label == NULL) {
        return;
    }
    for (i = 0U; (i + 1U < cap) && (label[i] != '\0'); i++) {
        dst[i] = label[i];
    }
    dst[i] = '\0';
}

static void ota_fill_partition_labels(ota_status_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t  *run_desc;

    if (out == NULL) {
        return;
    }
    out->run_label[0]    = '\0';
    out->target_label[0] = '\0';
    out->run_version[0]  = '\0';
    ota_copy_label(out->run_label, sizeof(out->run_label), (run != NULL) ? run->label : NULL);
    ota_copy_label(out->target_label, sizeof(out->target_label), (tgt != NULL) ? tgt->label : NULL);
    run_desc = esp_app_get_description();
    if (run_desc != NULL) {
        ota_copy_label(out->run_version, sizeof(out->run_version), run_desc->version);
    }
}

status_t ota_get_status(ota_status_t *out)
{
    status_t st;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    st = ota_lock();
    if (st != ESP_OK) {
        return st;
    }
    (void)memset(out, 0, sizeof(*out));
    out->state         = s_state;
    if (s_cloud_pull && (s_state == OTA_SESSION_WRITING)) {
        out->state = OTA_SESSION_PULLING;
    }
    out->expected_size = s_expected;
    out->written       = s_written;
    (void)snprintf(out->pending_version, sizeof(out->pending_version), "%s", s_pending_version);
    ota_fill_partition_labels(out);
    ota_unlock();
    return ESP_OK;
}

status_t ota_upload_begin(size_t image_size, const uint8_t *header_peek, size_t header_len)
{
    const esp_partition_t *update;
    const esp_app_desc_t  *run_desc;
    esp_app_desc_t         new_desc;
    status_t               st;

    if ((header_peek == NULL) || (header_len < OTA_HEADER_PEEK_MIN) || (image_size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    st = ota_lock();
    if (st != ESP_OK) {
        return st;
    }

    if (s_state != OTA_SESSION_IDLE) {
        if (s_state == OTA_SESSION_WRITING) {
            (void)esp_ota_abort(s_ota_handle);
        }
        ota_reset_session();
    }

    update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        ota_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size > update->size) {
        ESP_LOGW(TAG, "image %u exceeds partition %s size %u", (unsigned)image_size, update->label,
                 (unsigned)update->size);
        ota_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    if (!ota_find_app_desc(header_peek, header_len, &new_desc)) {
        ESP_LOGW(TAG, "app descriptor not found in first %u bytes", (unsigned)header_len);
        ota_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    run_desc = esp_app_get_description();
    if ((run_desc != NULL) && !ota_version_is_greater(new_desc.version, run_desc->version)) {
        ESP_LOGW(TAG, "reject downgrade/new equal: new=%s run=%s", new_desc.version, run_desc->version);
        ota_unlock();
        return ESP_ERR_INVALID_VERSION;
    }

    st = esp_ota_begin(update, image_size, &s_ota_handle);
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(st));
        ota_reset_session();
        ota_unlock();
        return st;
    }

    st = esp_ota_write(s_ota_handle, header_peek, header_len);
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write (header) failed: %s", esp_err_to_name(st));
        (void)esp_ota_abort(s_ota_handle);
        ota_reset_session();
        ota_unlock();
        return st;
    }

    s_target = update;
    s_expected = image_size;
    s_written  = header_len;
    s_state    = OTA_SESSION_WRITING;
    (void)snprintf(s_pending_version, sizeof(s_pending_version), "%s", new_desc.version);
    (void)mbedtls_sha256_init(&s_sha256);
    (void)mbedtls_sha256_starts(&s_sha256, 0);
    s_sha256_active = true;
    ota_sha256_update(header_peek, header_len);
    ESP_LOGI(TAG, "begin -> %s size=%u ver=%s", update->label, (unsigned)image_size, new_desc.version);
    ota_unlock();
    return ESP_OK;
}

status_t ota_upload_write(const uint8_t *data, size_t len)
{
    status_t st;

    if ((data == NULL) || (len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    st = ota_lock();
    if (st != ESP_OK) {
        return st;
    }

    if (s_state != OTA_SESSION_WRITING) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if ((s_written + len) > s_expected) {
        ota_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    st = esp_ota_write(s_ota_handle, data, len);
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(st));
        (void)esp_ota_abort(s_ota_handle);
        ota_reset_session();
        ota_unlock();
        return st;
    }

    s_written += len;
    ota_sha256_update(data, len);
    ota_unlock();
    return ESP_OK;
}

status_t ota_upload_end(void)
{
    status_t st;

    st = ota_lock();
    if (st != ESP_OK) {
        return st;
    }

    if (s_state != OTA_SESSION_WRITING) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_written != s_expected) {
        ESP_LOGW(TAG, "size mismatch written=%u expected=%u", (unsigned)s_written, (unsigned)s_expected);
        ota_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    st = esp_ota_end(s_ota_handle);
    s_ota_handle = 0;
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(st));
        ota_reset_session();
        ota_unlock();
        return st;
    }

    st = ota_sha256_verify();
    if (st != ESP_OK) {
        ota_reset_session();
        ota_unlock();
        return st;
    }

    s_state = OTA_SESSION_READY;
    ESP_LOGI(TAG, "end OK -> %s ver=%s (await apply)", s_target->label, s_pending_version);
    ota_unlock();
    return ESP_OK;
}

status_t ota_upload_abort(void)
{
    status_t st;

    st = ota_lock();
    if (st != ESP_OK) {
        return st;
    }

    if (s_state == OTA_SESSION_IDLE) {
        ota_unlock();
        return ESP_OK;
    }

    if (s_state == OTA_SESSION_WRITING) {
        (void)esp_ota_abort(s_ota_handle);
        ESP_LOGI(TAG, "aborted writing session at %u/%u bytes", (unsigned)s_written, (unsigned)s_expected);
    } else if (s_state == OTA_SESSION_READY) {
        ESP_LOGI(TAG, "aborted ready session (no reboot)");
    }

    ota_reset_session();
    ota_unlock();
    return ESP_OK;
}

status_t ota_apply(void)
{
    const esp_partition_t *target;
    status_t               st;

    st = ota_lock();
    if (st != ESP_OK) {
        return st;
    }

    if (s_state != OTA_SESSION_READY) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    target = s_target;
    if (target == NULL) {
        ota_reset_session();
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    st = esp_ota_set_boot_partition(target);
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(st));
        ota_unlock();
        return st;
    }

    ESP_LOGI(TAG, "apply -> boot %s, restarting", target->label);
    ota_reset_session();
    ota_unlock();
    esp_restart();
}

status_t ota_confirm_running_image(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t     state;
    status_t                 st;

    if (run == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    st = esp_ota_get_state_partition(run, &state);
    if (st != ESP_OK) {
        ESP_LOGW(TAG, "get_state_partition: %s", esp_err_to_name(st));
        return st;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    st = esp_ota_mark_app_valid_cancel_rollback();
    if (st != ESP_OK) {
        ESP_LOGE(TAG, "mark_app_valid failed: %s", esp_err_to_name(st));
        return st;
    }

    ESP_LOGI(TAG, "running image on %s marked valid", run->label);
    return ESP_OK;
}

status_t ota_upload_set_expected_sha256_hex(const char *sha256_hex)
{
    status_t st;

    st = ota_lock();
    if (st != ESP_OK) {
        return st;
    }
    if (s_state != OTA_SESSION_IDLE) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_expect_sha256 = false;
    if ((sha256_hex != NULL) && (sha256_hex[0] != '\0')) {
        if (!ota_parse_sha256_hex(sha256_hex, s_expected_sha256)) {
            ota_unlock();
            return ESP_ERR_INVALID_ARG;
        }
        s_expect_sha256 = true;
    }
    ota_unlock();
    return ESP_OK;
}

status_t ota_session_begin_cloud_pull(void)
{
    status_t st;

    st = ota_lock();
    if (st != ESP_OK) {
        return st;
    }
    if (s_state != OTA_SESSION_IDLE) {
        if (s_state == OTA_SESSION_WRITING) {
            (void)esp_ota_abort(s_ota_handle);
        }
        ota_reset_session();
    }
    s_cloud_pull = true;
    ota_unlock();
    return ESP_OK;
}

void ota_session_end_cloud_pull(void)
{
    if (ota_lock() != ESP_OK) {
        return;
    }
    s_cloud_pull = false;
    ota_unlock();
}
