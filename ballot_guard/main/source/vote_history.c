/**
 * @file vote_history.c
 * @brief 逐条投票历史 FIFO（最多 20 条，满则覆盖最旧）。
 */

#include "vote_history.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "ds3231.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "vote_menu_zh.h"

#define TAG "vote_hist"
#define VOTE_NVS_NS "ballot_guard"
#define KEY_HIST_COUNT "vh_cnt"
#define KEY_HIST_FMT "vh_%02u"

static vote_history_entry_t s_entries[VOTE_HISTORY_MAX_RECORDS];
static uint8_t s_count;

static bool stamp_now(uint32_t *date_ymd, uint32_t *time_hms)
{
    ds3231_t *rtc = BoardDs3231();
    ds3231_datetime_t dt;

    if (date_ymd == NULL || time_hms == NULL) {
        return false;
    }

    if (rtc != NULL && ds3231_read_datetime(rtc, &dt) == DS3231_OK) {
        *date_ymd = (uint32_t)dt.year * 10000U + (uint32_t)dt.month * 100U + (uint32_t)dt.day;
        *time_hms = (uint32_t)dt.hour * 10000U + (uint32_t)dt.minute * 100U + (uint32_t)dt.second;
        return true;
    }

    *date_ymd = 0U;
    *time_hms = 0U;
    return false;
}

static bool history_push(const vote_history_entry_t *entry)
{
    if (entry == NULL) {
        return false;
    }

    if (s_count < VOTE_HISTORY_MAX_RECORDS) {
        s_entries[s_count] = *entry;
        s_count++;
    } else {
        (void)memmove(&s_entries[0], &s_entries[1],
                      (size_t)(VOTE_HISTORY_MAX_RECORDS - 1U) * sizeof(vote_history_entry_t));
        s_entries[VOTE_HISTORY_MAX_RECORDS - 1U] = *entry;
    }

    return true;
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

    if (nvs_set_u8(h, KEY_HIST_COUNT, s_count) != ESP_OK) {
        ok = false;
    }

    for (i = 0U; i < s_count; i++) {
        char key[8];
        (void)snprintf(key, sizeof(key), KEY_HIST_FMT, (unsigned)i);
        err = nvs_set_blob(h, key, &s_entries[i], sizeof(vote_history_entry_t));
        if (err != ESP_OK) {
            ok = false;
            break;
        }
    }

    for (i = s_count; i < VOTE_HISTORY_MAX_RECORDS; i++) {
        char key[8];
        (void)snprintf(key, sizeof(key), KEY_HIST_FMT, (unsigned)i);
        (void)nvs_erase_key(h, key);
    }

    if (ok && nvs_commit(h) != ESP_OK) {
        ok = false;
    }

    nvs_close(h);
    return ok;
}

void vote_history_init(void)
{
    nvs_handle_t h;
    esp_err_t err;
    uint8_t i;

    s_count = 0U;
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
        char key[8];
        size_t len = sizeof(vote_history_entry_t);
        (void)snprintf(key, sizeof(key), KEY_HIST_FMT, (unsigned)i);
        err = nvs_get_blob(h, key, &s_entries[i], &len);
        if (err != ESP_OK || len != sizeof(vote_history_entry_t)) {
            s_count = i;
            break;
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "loaded %u vote records", (unsigned)s_count);
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

static bool append_entry(uint8_t kind, vote_spoiled_type_e stype, uint8_t cand_idx)
{
    vote_history_entry_t entry;

    (void)memset(&entry, 0, sizeof(entry));
    (void)stamp_now(&entry.date_ymd, &entry.time_hms);
    entry.kind         = kind;
    entry.spoiled_type = (uint8_t)stype;
    entry.cand_idx     = cand_idx;
    if (kind == VOTE_HISTORY_KIND_VALID && cand_idx < VOTE_STATUS_MAX_CANDIDATES) {
        (void)snprintf(entry.cand_name, sizeof(entry.cand_name), "%s", vote_status_candidate_lcd_name(cand_idx));
    }

    if (!history_push(&entry)) {
        return false;
    }
    if (!hist_save_all()) {
        ESP_LOGW(TAG, "history save failed");
        return false;
    }

    ESP_LOGI(TAG, "record kind=%u idx=%u (total %u)", (unsigned)kind, (unsigned)cand_idx, (unsigned)s_count);
    return true;
}

bool vote_history_append_valid(uint8_t cand_idx)
{
    if (cand_idx >= vote_status_candidate_count()) {
        return false;
    }
    return append_entry(VOTE_HISTORY_KIND_VALID, VOTE_SPOILED_NONE, cand_idx);
}

bool vote_history_append_spoiled(vote_spoiled_type_e type, uint8_t cand_idx)
{
    if (type == VOTE_SPOILED_NONE) {
        type = VOTE_SPOILED_IRREGULAR;
    }
    return append_entry(VOTE_HISTORY_KIND_SPOILED, type, cand_idx);
}

bool vote_history_append_current(void)
{
    return false;
}

bool vote_history_archive_session_if_needed(void)
{
    return false;
}

void vote_history_on_session_reset(void)
{
}

bool vote_history_clear_all(void)
{
    nvs_handle_t h;
    esp_err_t err;
    uint8_t i;
    bool ok = true;

    s_count = 0U;
    (void)memset(s_entries, 0, sizeof(s_entries));

    err = nvs_open(VOTE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return false;
    }

    if (nvs_set_u8(h, KEY_HIST_COUNT, 0U) != ESP_OK) {
        ok = false;
    }
    for (i = 0U; i < VOTE_HISTORY_MAX_RECORDS; i++) {
        char key[8];
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

void vote_history_format_time(const vote_history_entry_t *e, char *buf, size_t cap)
{
    uint32_t h;
    uint32_t m;
    uint32_t s;

    if (e == NULL || buf == NULL || cap == 0U) {
        return;
    }

    if (e->time_hms == 0U && e->date_ymd == 0U) {
        (void)snprintf(buf, cap, "--:--:--");
        return;
    }

    h = e->time_hms / 10000U;
    m = (e->time_hms / 100U) % 100U;
    s = e->time_hms % 100U;
    (void)snprintf(buf, cap, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

void vote_history_format_detail(const vote_history_entry_t *e, char *buf, size_t cap)
{
    const char *detail = VOTE_ZH_IRREGULAR;

    if (e == NULL || buf == NULL || cap == 0U) {
        return;
    }

    if (e->kind == VOTE_HISTORY_KIND_VALID) {
        if (e->cand_name[0] != '\0') {
            (void)snprintf(buf, cap, "%s %s", VOTE_ZH_VALID, e->cand_name);
        } else {
            (void)snprintf(buf, cap, "%s", VOTE_ZH_VALID);
        }
        return;
    }

    switch ((vote_spoiled_type_e)e->spoiled_type) {
    case VOTE_SPOILED_BLANK:
        detail = VOTE_ZH_BLANK;
        break;
    case VOTE_SPOILED_MULTIPLE:
        detail = VOTE_ZH_MULTIPLE;
        break;
    case VOTE_SPOILED_IRREGULAR:
    default:
        detail = VOTE_ZH_IRREGULAR;
        break;
    }
    (void)snprintf(buf, cap, "%s %s", VOTE_ZH_SPOILED, detail);
}

void vote_history_format_title(const vote_history_entry_t *e, char *buf, size_t cap)
{
    vote_history_format_time(e, buf, cap);
}

void vote_history_format_summary(const vote_history_entry_t *e, char *buf, size_t cap)
{
    vote_history_format_detail(e, buf, cap);
}

size_t vote_history_build_json(char *out, size_t out_cap)
{
    size_t off = 0U;
    size_t n;
    uint8_t i;
    const vote_history_entry_t *e;
    char time_buf[16];
    char date_buf[16];
    uint32_t y;
    uint32_t mo;
    uint32_t d;

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

        vote_history_format_time(e, time_buf, sizeof(time_buf));

        if (e->date_ymd != 0U) {
            y  = e->date_ymd / 10000U;
            mo = (e->date_ymd / 100U) % 100U;
            d  = e->date_ymd % 100U;
            (void)snprintf(date_buf, sizeof(date_buf), "%04lu-%02lu-%02lu", (unsigned long)y, (unsigned long)mo,
                           (unsigned long)d);
        } else {
            (void)snprintf(date_buf, sizeof(date_buf), "--");
        }

        n = (size_t)snprintf(out + off, out_cap - off,
                             "%s{\"kind\":%u,\"spoiled_type\":%u,\"time\":\"%s\",\"date\":\"%s\","
                             "\"candidate\":\"%s\"}",
                             (i > 0U) ? "," : "", (unsigned)e->kind, (unsigned)e->spoiled_type, time_buf, date_buf,
                             e->cand_name);
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
