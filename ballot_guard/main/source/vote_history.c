/**
 * @file vote_history.c
 * @brief 投票历史 FIFO（最多 20 条，满则覆盖最旧）。
 */

#include "vote_history.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "vote_menu_pages.h"
#include "vote_status.h"

extern uint32_t nvs_firmware_build_date(void);

#define TAG "vote_hist"
#define VOTE_NVS_NS "ballot_guard"
#define KEY_HIST_COUNT "hist_count"
#define KEY_HIST_FMT "hist_%u"

static vote_history_entry_t s_entries[VOTE_HISTORY_MAX_RECORDS];
static uint8_t s_count;
static bool s_session_archived;

static uint32_t pack_end_stamp(uint8_t end_h, uint8_t end_m)
{
    uint32_t ymd = nvs_firmware_build_date();
    if (ymd == 0U) {
        ymd = 20260621U;
    }
    return ymd * 10000U + (uint32_t)end_h * 100U + (uint32_t)end_m;
}

static bool hist_save_all(void)
{
    nvs_handle_t h;
    esp_err_t err;
    uint8_t i;
    bool ok = true;

    err = nvs_open(VOTE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_set_u8(h, KEY_HIST_COUNT, s_count);
    if (err != ESP_OK) {
        ok = false;
    }

    for (i = 0U; i < s_count; i++) {
        char key[12];
        (void)snprintf(key, sizeof(key), KEY_HIST_FMT, (unsigned)i);
        err = nvs_set_blob(h, key, &s_entries[i], sizeof(vote_history_entry_t));
        if (err != ESP_OK) {
            ok = false;
            break;
        }
    }

    if (ok) {
        err = nvs_commit(h);
        ok  = (err == ESP_OK);
    }

    nvs_close(h);
    return ok;
}

void vote_history_init(void)
{
    nvs_handle_t h;
    esp_err_t err;
    uint8_t i;

    s_count            = 0U;
    s_session_archived = false;
    (void)memset(s_entries, 0, sizeof(s_entries));

    err = nvs_open(VOTE_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return;
    }

    err = nvs_get_u8(h, KEY_HIST_COUNT, &s_count);
    if (err != ESP_OK || s_count > VOTE_HISTORY_MAX_RECORDS) {
        s_count = 0U;
        nvs_close(h);
        return;
    }

    for (i = 0U; i < s_count; i++) {
        char key[12];
        size_t len = sizeof(vote_history_entry_t);
        (void)snprintf(key, sizeof(key), KEY_HIST_FMT, (unsigned)i);
        err = nvs_get_blob(h, key, &s_entries[i], &len);
        if (err != ESP_OK || len != sizeof(vote_history_entry_t)) {
            s_count = i;
            break;
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "loaded %u history records", (unsigned)s_count);
}

uint8_t vote_history_count(void)
{
    return s_count;
}

bool vote_history_get_display(uint8_t display_idx, const vote_history_entry_t **out)
{
    if (out == NULL || display_idx >= s_count) {
        return false;
    }
    *out = &s_entries[(size_t)(s_count - 1U - display_idx)];
    return true;
}

bool vote_history_append_current(void)
{
    vote_history_entry_t entry;
    vote_menu_settings_t *st = vote_menu_settings();
    uint8_t count;
    uint8_t i;

    if (st == NULL) {
        return false;
    }

    count = vote_status_candidate_count();
    (void)memset(&entry, 0, sizeof(entry));
    entry.end_stamp = pack_end_stamp(st->end_h, st->end_m);
    entry.valid     = vote_status_valid_total();
    entry.spoiled   = vote_status_spoiled();

    for (i = 0U; i < count && i < VOTE_STATUS_MAX_CANDIDATES; i++) {
        entry.cand_votes[i] = vote_status_votes(i);
        (void)snprintf(entry.names_snapshot[i], sizeof(entry.names_snapshot[i]), "%s",
                       vote_status_candidate_lcd_name(i));
    }

    if (s_count < VOTE_HISTORY_MAX_RECORDS) {
        s_entries[s_count] = entry;
        s_count++;
    } else {
        (void)memmove(&s_entries[0], &s_entries[1], (size_t)(VOTE_HISTORY_MAX_RECORDS - 1U) * sizeof(entry));
        s_entries[VOTE_HISTORY_MAX_RECORDS - 1U] = entry;
    }

    if (!hist_save_all()) {
        ESP_LOGW(TAG, "history save failed");
        return false;
    }

    ESP_LOGI(TAG, "archived session V=%u S=%u (total %u)", (unsigned)entry.valid, (unsigned)entry.spoiled,
             (unsigned)s_count);
    return true;
}

bool vote_history_archive_session_if_needed(void)
{
    if (s_session_archived) {
        return false;
    }
    if (!vote_status_has_any_votes()) {
        return false;
    }
    if (!vote_history_append_current()) {
        return false;
    }
    s_session_archived = true;
    return true;
}

void vote_history_on_session_reset(void)
{
    s_session_archived = false;
}

bool vote_history_clear_all(void)
{
    nvs_handle_t h;
    esp_err_t err;
    uint8_t i;
    bool ok = true;

    s_count            = 0U;
    s_session_archived = false;
    (void)memset(s_entries, 0, sizeof(s_entries));

    err = nvs_open(VOTE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return false;
    }

    if (nvs_set_u8(h, KEY_HIST_COUNT, 0U) != ESP_OK) {
        ok = false;
    }
    for (i = 0U; i < VOTE_HISTORY_MAX_RECORDS; i++) {
        char key[12];
        (void)snprintf(key, sizeof(key), KEY_HIST_FMT, (unsigned)i);
        (void)nvs_erase_key(h, key);
    }

    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK || !ok) {
        ESP_LOGW(TAG, "history clear failed");
        return false;
    }
    ESP_LOGI(TAG, "history cleared");
    return true;
}

void vote_history_format_title(const vote_history_entry_t *e, char *buf, size_t cap)
{
    uint32_t ymd_part;
    uint32_t hm_part;
    uint32_t y;
    uint32_t mo;
    uint32_t d;
    uint32_t h;
    uint32_t m;

    if (e == NULL || buf == NULL || cap == 0U) {
        return;
    }

    ymd_part = e->end_stamp / 10000U;
    hm_part  = e->end_stamp % 10000U;
    y        = ymd_part / 10000U;
    mo       = (ymd_part / 100U) % 100U;
    d        = ymd_part % 100U;
    h        = hm_part / 100U;
    m        = hm_part % 100U;
    (void)snprintf(buf, cap, "%04lu-%02lu-%02lu %02lu:%02lu End", (unsigned long)y, (unsigned long)mo,
                   (unsigned long)d, (unsigned long)h, (unsigned long)m);
}

void vote_history_format_summary(const vote_history_entry_t *e, char *buf, size_t cap)
{
    uint8_t i;
    size_t off = 0U;
    int n;

    if (e == NULL || buf == NULL || cap == 0U) {
        return;
    }

    n = snprintf(buf, cap, "V%u S%u", (unsigned)e->valid, (unsigned)e->spoiled);
    if (n <= 0 || (size_t)n >= cap) {
        return;
    }
    off = (size_t)n;

    for (i = 0U; i < VOTE_STATUS_MAX_CANDIDATES; i++) {
        if (e->cand_votes[i] == 0U && e->names_snapshot[i][0] == '\0') {
            continue;
        }
        if (e->cand_votes[i] == 0U) {
            continue;
        }
        n = snprintf(buf + off, cap - off, " %.*s%u", 5, e->names_snapshot[i], (unsigned)e->cand_votes[i]);
        if (n <= 0 || (size_t)(off + (size_t)n) >= cap) {
            break;
        }
        off += (size_t)n;
    }
}

size_t vote_history_build_json(char *out, size_t out_cap)
{
    size_t off = 0U;
    size_t n;
    uint8_t i;
    const vote_history_entry_t *e;
    char title[40];
    char summary[120];

    if (out == NULL || out_cap < 32U) {
        return 0U;
    }

    n = (size_t)snprintf(out, out_cap, "{\"ok\":true,\"count\":%u,\"records\":[", (unsigned)s_count);
    if (n <= 0U || n >= out_cap) {
        return 0U;
    }
    off = n;

    for (i = 0U; i < s_count; i++) {
        if (!vote_history_get_display(i, &e) || e == NULL) {
            break;
        }
        vote_history_format_title(e, title, sizeof(title));
        vote_history_format_summary(e, summary, sizeof(summary));
        n = (size_t)snprintf(out + off, out_cap - off,
                             "%s{\"end_stamp\":%lu,\"title\":\"%s\",\"summary\":\"%s\",\"valid\":%u,\"spoiled\":%u}",
                             (i > 0U) ? "," : "", (unsigned long)e->end_stamp, title, summary, (unsigned)e->valid,
                             (unsigned)e->spoiled);
        if (n <= 0U || (off + n >= out_cap)) {
            return 0U;
        }
        off += n;
    }

    n = (size_t)snprintf(out + off, out_cap - off, "]}");
    if (n <= 0U || (off + n >= out_cap)) {
        return 0U;
    }
    off += n;
    return off;
}
