/**
 * @file vote_nvs.c
 * @brief ballot_guard NVS 持久化（命名空间 ballot_guard，见评审稿 §9.3）。
 */

#include "vote_nvs.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "vote_menu_pages.h"
#include "vote_history.h"

#define TAG "vote_nvs"
#define VOTE_NVS_NS "ballot_guard"

#define KEY_CFG_START_H "cfg_start_h"
#define KEY_CFG_START_M "cfg_start_m"
#define KEY_CFG_END_H "cfg_end_h"
#define KEY_CFG_END_M "cfg_end_m"
#define KEY_CFG_CAND_COUNT "cfg_cand_count"
/** ESP-IDF NVS 键名最长 15 字符；旧键 cfg_cooldown_sec(16) 无效，仅作读取兼容。 */
#define KEY_CFG_COOLDOWN "cfg_cooldown"
#define KEY_CFG_COOLDOWN_LEGACY "cfg_cooldown_sec"
#define KEY_CFG_MAGIC "cfg_magic"
#define CFG_MAGIC_VALUE (0x42475601U)

#define KEY_CAND_NAME_FMT "cand_name_%u"
#define KEY_VOTE_VALID "vote_valid"
#define KEY_VOTE_SPOILED "vote_spoiled"
#define KEY_VOTE_CAND_FMT "vote_cand_%u"

static bool s_loaded;

static void default_candidate_name(uint8_t idx, char *out, size_t cap)
{
    if (out == NULL || cap == 0U) {
        return;
    }
    if (idx < 26U && cap >= 2U) {
        out[0] = (char)('A' + idx);
        out[1] = '\0';
        return;
    }
    (void)snprintf(out, cap, "Candidate %u", (unsigned)(idx + 1U));
}

static bool vote_nvs_read_u8(nvs_handle_t h, const char *key, uint8_t *out, uint8_t def)
{
    esp_err_t err = nvs_get_u8(h, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = def;
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read u8 %s failed: %s", key, esp_err_to_name(err));
        *out = def;
        return false;
    }
    return true;
}

static bool vote_nvs_read_u32(nvs_handle_t h, const char *key, uint32_t *out, uint32_t def)
{
    esp_err_t err = nvs_get_u32(h, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = def;
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read u32 %s failed: %s", key, esp_err_to_name(err));
        *out = def;
        return false;
    }
    return true;
}

static void clamp_cfg(vote_nvs_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    if (cfg->candidate_count < 2U) {
        cfg->candidate_count = 2U;
    }
    if (cfg->candidate_count > VOTE_STATUS_MAX_CANDIDATES) {
        cfg->candidate_count = VOTE_STATUS_MAX_CANDIDATES;
    }
    if (cfg->cooldown_sec < 3U) {
        cfg->cooldown_sec = 3U;
    }
    if (cfg->cooldown_sec > 10U) {
        cfg->cooldown_sec = 10U;
    }
}

static void default_cfg(vote_nvs_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->start_h         = 8U;
    cfg->start_m         = 0U;
    cfg->end_h           = 16U;
    cfg->end_m           = 0U;
    cfg->candidate_count = 3U;
    cfg->cooldown_sec    = 5U;
    clamp_cfg(cfg);
}

static void apply_cfg_to_settings(const vote_nvs_cfg_t *cfg)
{
    vote_menu_settings_t *st = vote_menu_settings();

    if (cfg == NULL || st == NULL) {
        return;
    }
    st->start_h         = cfg->start_h;
    st->start_m         = cfg->start_m;
    st->end_h           = cfg->end_h;
    st->end_m           = cfg->end_m;
    st->candidate_count = cfg->candidate_count;
    st->cooldown_sec    = cfg->cooldown_sec;
}

static bool load_cfg_from_handle(nvs_handle_t h, vote_nvs_cfg_t *out)
{
    uint8_t legacy_cd = 5U;
    uint32_t magic    = 0U;

    if (out == NULL) {
        return false;
    }

    (void)vote_nvs_read_u8(h, KEY_CFG_START_H, &out->start_h, 8U);
    (void)vote_nvs_read_u8(h, KEY_CFG_START_M, &out->start_m, 0U);
    (void)vote_nvs_read_u8(h, KEY_CFG_END_H, &out->end_h, 16U);
    (void)vote_nvs_read_u8(h, KEY_CFG_END_M, &out->end_m, 0U);
    (void)vote_nvs_read_u8(h, KEY_CFG_CAND_COUNT, &out->candidate_count, 3U);

    if (!vote_nvs_read_u8(h, KEY_CFG_COOLDOWN, &out->cooldown_sec, 0U) || out->cooldown_sec == 0U) {
        (void)vote_nvs_read_u8(h, KEY_CFG_COOLDOWN_LEGACY, &legacy_cd, 5U);
        out->cooldown_sec = legacy_cd;
    }

    clamp_cfg(out);
    (void)vote_nvs_read_u32(h, KEY_CFG_MAGIC, &magic, 0U);

    ESP_LOGI(TAG,
             "cfg %02u:%02u-%02u:%02u count=%u cd=%us magic=0x%08lx",
             (unsigned)out->start_h,
             (unsigned)out->start_m,
             (unsigned)out->end_h,
             (unsigned)out->end_m,
             (unsigned)out->candidate_count,
             (unsigned)out->cooldown_sec,
             (unsigned long)magic);
    return true;
}

bool vote_nvs_validate_candidate_name(const char *name)
{
    size_t i;
    size_t len;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    len = strlen(name);
    if (len > VOTE_NVS_CAND_NAME_MAX) {
        return false;
    }
    for (i = 0U; i < len; i++) {
        const char c = name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-') {
            continue;
        }
        return false;
    }
    return true;
}

void vote_nvs_init(void)
{
    nvs_handle_t h;
    esp_err_t err;
    vote_nvs_cfg_t cfg;
    vote_menu_settings_t *st;
    uint8_t i;

    if (s_loaded) {
        return;
    }

    vote_status_init_defaults();

    err = nvs_open(VOTE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s, use defaults", VOTE_NVS_NS, esp_err_to_name(err));
        s_loaded = true;
        return;
    }

    (void)load_cfg_from_handle(h, &cfg);
    apply_cfg_to_settings(&cfg);
    st = vote_menu_settings();
    (void)st;

    for (i = 0U; i < VOTE_STATUS_MAX_CANDIDATES; i++) {
        char key[16];
        char name[VOTE_NVS_CAND_NAME_BUF];
        size_t len = sizeof(name);

        (void)snprintf(key, sizeof(key), KEY_CAND_NAME_FMT, (unsigned)i);
        err = nvs_get_str(h, key, name, &len);
        if (err != ESP_OK) {
            default_candidate_name(i, name, sizeof(name));
        }
        vote_status_set_candidate_name(i, name);
    }

    {
        uint32_t valid   = 0U;
        uint32_t spoiled = 0U;
        uint32_t votes[VOTE_STATUS_MAX_CANDIDATES];
        uint8_t count = vote_status_candidate_count();

        (void)vote_nvs_read_u32(h, KEY_VOTE_VALID, &valid, 0U);
        (void)vote_nvs_read_u32(h, KEY_VOTE_SPOILED, &spoiled, 0U);
        for (i = 0U; i < VOTE_STATUS_MAX_CANDIDATES; i++) {
            char vkey[16];
            (void)snprintf(vkey, sizeof(vkey), KEY_VOTE_CAND_FMT, (unsigned)i);
            (void)vote_nvs_read_u32(h, vkey, &votes[i], 0U);
        }
        vote_status_load_counts((uint16_t)valid, (uint16_t)spoiled, votes, count);
    }

    nvs_close(h);
    s_loaded = true;

    vote_history_init();

    ESP_LOGI(TAG, "NVS loaded (%u candidates)", (unsigned)vote_status_candidate_count());
}

bool vote_nvs_reload_settings(void)
{
    nvs_handle_t h;
    esp_err_t err;
    vote_nvs_cfg_t cfg;

    err = nvs_open(VOTE_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }
    (void)load_cfg_from_handle(h, &cfg);
    nvs_close(h);
    apply_cfg_to_settings(&cfg);
    return true;
}

bool vote_nvs_load_cfg(vote_nvs_cfg_t *out)
{
    nvs_handle_t h;
    esp_err_t err;

    if (out == NULL) {
        return false;
    }

    err = nvs_open(VOTE_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        default_cfg(out);
        return false;
    }

    (void)load_cfg_from_handle(h, out);
    nvs_close(h);
    return true;
}

bool vote_nvs_save_cfg(const vote_nvs_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err;
    bool ok = true;
    vote_nvs_cfg_t local;

    if (cfg == NULL) {
        return false;
    }

    local = *cfg;
    clamp_cfg(&local);

    err = nvs_open(VOTE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_cfg open failed: %s", esp_err_to_name(err));
        return false;
    }

    if (nvs_set_u8(h, KEY_CFG_START_H, local.start_h) != ESP_OK) {
        ok = false;
    }
    if (nvs_set_u8(h, KEY_CFG_START_M, local.start_m) != ESP_OK) {
        ok = false;
    }
    if (nvs_set_u8(h, KEY_CFG_END_H, local.end_h) != ESP_OK) {
        ok = false;
    }
    if (nvs_set_u8(h, KEY_CFG_END_M, local.end_m) != ESP_OK) {
        ok = false;
    }
    if (nvs_set_u8(h, KEY_CFG_CAND_COUNT, local.candidate_count) != ESP_OK) {
        ok = false;
    }
    if (nvs_set_u8(h, KEY_CFG_COOLDOWN, local.cooldown_sec) != ESP_OK) {
        ok = false;
    }
    if (nvs_set_u32(h, KEY_CFG_MAGIC, CFG_MAGIC_VALUE) != ESP_OK) {
        ok = false;
    }

    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK || !ok) {
        ESP_LOGE(TAG, "cfg save failed commit=%s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG,
             "cfg saved %02u:%02u-%02u:%02u count=%u cd=%us",
             (unsigned)local.start_h,
             (unsigned)local.start_m,
             (unsigned)local.end_h,
             (unsigned)local.end_m,
             (unsigned)local.candidate_count,
             (unsigned)local.cooldown_sec);
    return true;
}

bool vote_nvs_get_candidate_name(uint8_t idx, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0U || idx >= VOTE_STATUS_MAX_CANDIDATES) {
        return false;
    }
    (void)snprintf(out, out_cap, "%s", vote_status_candidate_lcd_name(idx));
    return true;
}

bool vote_nvs_set_candidate_name(uint8_t idx, const char *name)
{
    nvs_handle_t h;
    esp_err_t err;
    char key[16];

    if (idx >= VOTE_STATUS_MAX_CANDIDATES || !vote_nvs_validate_candidate_name(name)) {
        return false;
    }

    vote_status_set_candidate_name(idx, name);

    err = nvs_open(VOTE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cand name open failed");
        return false;
    }

    (void)snprintf(key, sizeof(key), KEY_CAND_NAME_FMT, (unsigned)idx);
    err = nvs_set_str(h, key, name);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cand name save failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool vote_nvs_set_candidate_names(const char *const *names, uint8_t count)
{
    uint8_t i;

    if (names == NULL || count == 0U || count > VOTE_STATUS_MAX_CANDIDATES) {
        return false;
    }
    for (i = 0U; i < count; i++) {
        if (!vote_nvs_set_candidate_name(i, names[i])) {
            return false;
        }
    }
    return true;
}

bool vote_nvs_save_votes(void)
{
    nvs_handle_t h;
    esp_err_t err;
    bool ok = true;
    uint8_t i;
    uint8_t count = vote_status_candidate_count();
    uint16_t valid = 0U;

    err = nvs_open(VOTE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return false;
    }

    for (i = 0U; i < count; i++) {
        valid = (uint16_t)(valid + vote_status_votes(i));
    }

    if (nvs_set_u32(h, KEY_VOTE_VALID, (uint32_t)valid) != ESP_OK) {
        ok = false;
    }
    if (nvs_set_u32(h, KEY_VOTE_SPOILED, (uint32_t)vote_status_spoiled()) != ESP_OK) {
        ok = false;
    }

    for (i = 0U; i < VOTE_STATUS_MAX_CANDIDATES; i++) {
        char vkey[16];
        (void)snprintf(vkey, sizeof(vkey), KEY_VOTE_CAND_FMT, (unsigned)i);
        if (nvs_set_u32(h, vkey, (uint32_t)vote_status_votes(i)) != ESP_OK) {
            ok = false;
        }
    }

    err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK) && ok;
}

bool vote_nvs_restore_default_cfg(void)
{
    vote_nvs_cfg_t cfg;

    default_cfg(&cfg);
    apply_cfg_to_settings(&cfg);
    if (!vote_nvs_save_cfg(&cfg)) {
        ESP_LOGE(TAG, "restore default cfg failed");
        return false;
    }
    ESP_LOGI(TAG, "cfg restored to factory defaults");
    return true;
}

bool vote_nvs_restore_default_candidate_names(void)
{
    uint8_t i;
    char name[VOTE_NVS_CAND_NAME_BUF];

    for (i = 0U; i < VOTE_STATUS_MAX_CANDIDATES; i++) {
        default_candidate_name(i, name, sizeof(name));
        if (!vote_nvs_set_candidate_name(i, name)) {
            ESP_LOGE(TAG, "restore default cand %u failed", (unsigned)i);
            return false;
        }
    }
    ESP_LOGI(TAG, "candidate names restored to defaults");
    return true;
}
