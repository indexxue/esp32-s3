/**
 * @file vote_status.c
 * @brief 投票看板 JSON 快照：时段、票数、外设状态（与 LCD 状态机逐步对齐）。
 */

#include "vote_status.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device_profile.h"
#include "board.h"
#include "ds3231.h"
#include "net_wifi.h"
#include "vote_menu_pages.h"
#include "vote_nvs.h"
#include "vote_history.h"

#define VOTE_STATUS_JSON_MAX (2048U)
#define VOTE_STATUS_NAME_LEN (VOTE_NVS_CAND_NAME_BUF)

static char s_names[VOTE_STATUS_MAX_CANDIDATES][VOTE_STATUS_NAME_LEN];

static uint16_t s_votes[VOTE_STATUS_MAX_CANDIDATES];
static uint16_t s_spoiled;

static char s_event_text[VOTE_STATUS_MAX_EVENTS][48];
static char s_event_time[VOTE_STATUS_MAX_EVENTS][12];
static uint8_t s_event_count;

static void default_name(uint8_t idx, char *out, size_t cap)
{
    (void)snprintf(out, cap, "Candidate %u", (unsigned)(idx + 1U));
}

static void init_default_names(void)
{
    uint8_t i;

    for (i = 0U; i < VOTE_STATUS_MAX_CANDIDATES; i++) {
        default_name(i, s_names[i], sizeof(s_names[i]));
    }
}

void vote_status_init_defaults(void)
{
    init_default_names();
}

static bool vote_status_read_datetime(ds3231_datetime_t *dt)
{
    ds3231_t *rtc = BoardDs3231();

    if (rtc == NULL || dt == NULL) {
        return false;
    }
    return (ds3231_read_datetime(rtc, dt) == DS3231_OK);
}

bool vote_status_format_clock(char *buf, size_t cap)
{
    ds3231_datetime_t dt;

    if (buf == NULL || cap == 0U) {
        return false;
    }
    if (!vote_status_read_datetime(&dt)) {
        (void)snprintf(buf, cap, "--:--:--");
        return false;
    }
    (void)snprintf(buf, cap, "%02u:%02u:%02u", (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
    return true;
}

int vote_status_seconds_of_day(void)
{
    ds3231_datetime_t dt;

    if (!vote_status_read_datetime(&dt)) {
        return -1;
    }
    return (int)((unsigned)dt.hour * 3600U + (unsigned)dt.minute * 60U + (unsigned)dt.second);
}

static void format_clock_hms(char *buf, size_t cap)
{
    (void)vote_status_format_clock(buf, cap);
}

static const char *phase_to_str(const char *phase)
{
    if (phase == NULL) {
        return "idle";
    }
    return phase;
}

static const char *phase_label_of(const char *phase)
{
    if (strcmp(phase, "waiting") == 0) {
        return "待开启";
    }
    if (strcmp(phase, "voting") == 0) {
        return "投票进行中";
    }
    if (strcmp(phase, "locked") == 0) {
        return "投票已锁定";
    }
    if (strcmp(phase, "booting") == 0) {
        return "初始化中";
    }
    if (strcmp(phase, "fault") == 0) {
        return "系统故障";
    }
    return "待机";
}

static void compute_phase(const vote_menu_settings_t *st,
                          const char **out_phase,
                          const char **out_label,
                          int *out_countdown_sec)
{
    const int start_sec = ((int)st->start_h * 60 + (int)st->start_m) * 60;
    const int end_sec   = ((int)st->end_h * 60 + (int)st->end_m) * 60;
    const int now_sec   = vote_status_seconds_of_day();

    if (out_countdown_sec != NULL) {
        *out_countdown_sec = 0;
    }

    if (now_sec < 0) {
        *out_phase = "fault";
        *out_label = phase_label_of("fault");
        return;
    }

    if (start_sec >= end_sec) {
        *out_phase = "idle";
        *out_label = phase_label_of("idle");
        return;
    }

    if (now_sec < start_sec) {
        *out_phase = "waiting";
        *out_label = phase_label_of("waiting");
        if (out_countdown_sec != NULL) {
            *out_countdown_sec = start_sec - now_sec;
        }
        return;
    }

    if (now_sec >= end_sec) {
        *out_phase = "locked";
        *out_label = phase_label_of("locked");
        return;
    }

    *out_phase = "voting";
    *out_label = phase_label_of("voting");
    if (out_countdown_sec != NULL) {
        *out_countdown_sec = end_sec - now_sec;
    }
}

const char *vote_status_current_phase(int *countdown_sec)
{
    vote_menu_settings_t *st = vote_menu_settings();
    const char *phase;
    const char *label;

    if (st == NULL) {
        return "idle";
    }
    compute_phase(st, &phase, &label, countdown_sec);
    return phase;
}

vote_lcd_screen_id_t vote_status_lcd_screen_for_phase(const char *phase)
{
    if (phase == NULL) {
        return VOTE_LCD_SCREEN_HOME;
    }
    if (strcmp(phase, "voting") == 0) {
        return VOTE_LCD_SCREEN_VOTING;
    }
    if (strcmp(phase, "locked") == 0) {
        return VOTE_LCD_SCREEN_LOCKED;
    }
    return VOTE_LCD_SCREEN_HOME;
}

bool vote_status_is_voting_phase(const char *phase)
{
    if (phase == NULL) {
        return false;
    }
    return (strcmp(phase, "voting") == 0 || strcmp(phase, "selecting") == 0 || strcmp(phase, "cooldown") == 0 ||
            strcmp(phase, "violation") == 0);
}

void vote_status_push_event(const char *time_hms, const char *text)
{
    uint8_t i;

    if (text == NULL || text[0] == '\0') {
        return;
    }

    if (s_event_count < VOTE_STATUS_MAX_EVENTS) {
        i = s_event_count;
        s_event_count++;
    } else {
        for (i = (uint8_t)(VOTE_STATUS_MAX_EVENTS - 1U); i > 0U; i--) {
            (void)memcpy(s_event_text[i], s_event_text[i - 1U], sizeof(s_event_text[0]));
            (void)memcpy(s_event_time[i], s_event_time[i - 1U], sizeof(s_event_time[0]));
        }
        i = 0U;
    }

    (void)snprintf(s_event_time[i], sizeof(s_event_time[0]), "%s", (time_hms != NULL) ? time_hms : "--:--:--");
    (void)snprintf(s_event_text[i], sizeof(s_event_text[0]), "%s", text);
}

void vote_status_reset_counts(void)
{
    (void)memset(s_votes, 0, sizeof(s_votes));
    s_spoiled     = 0U;
    s_event_count = 0U;
    vote_history_on_session_reset();
    (void)vote_nvs_save_votes();
}

void vote_status_load_counts(uint16_t valid, uint16_t spoiled, const uint32_t *cand_votes, uint8_t count)
{
    uint8_t i;

    (void)valid;
    (void)memset(s_votes, 0, sizeof(s_votes));
    s_spoiled = spoiled;
    if (cand_votes == NULL) {
        return;
    }
    if (count > VOTE_STATUS_MAX_CANDIDATES) {
        count = VOTE_STATUS_MAX_CANDIDATES;
    }
    for (i = 0U; i < count; i++) {
        s_votes[i] = (uint16_t)cand_votes[i];
    }
}

uint8_t vote_status_candidate_count(void)
{
    vote_menu_settings_t *st = vote_menu_settings();
    uint8_t count            = (st != NULL) ? st->candidate_count : 3U;

    if (count < 2U) {
        count = 2U;
    }
    if (count > VOTE_STATUS_MAX_CANDIDATES) {
        count = VOTE_STATUS_MAX_CANDIDATES;
    }
    return count;
}

uint16_t vote_status_votes(uint8_t idx)
{
    if (idx >= VOTE_STATUS_MAX_CANDIDATES) {
        return 0U;
    }
    return s_votes[idx];
}

uint16_t vote_status_spoiled(void)
{
    return s_spoiled;
}

uint16_t vote_status_valid_total(void)
{
    uint8_t count = vote_status_candidate_count();
    uint8_t i;
    uint16_t valid = 0U;

    for (i = 0U; i < count; i++) {
        valid = (uint16_t)(valid + s_votes[i]);
    }
    return valid;
}

bool vote_status_has_any_votes(void)
{
    return (vote_status_valid_total() > 0U) || (s_spoiled > 0U);
}

const char *vote_status_candidate_name(uint8_t idx)
{
    if (idx >= VOTE_STATUS_MAX_CANDIDATES) {
        return "?";
    }
    return s_names[idx];
}

const char *vote_status_candidate_lcd_name(uint8_t idx)
{
    return vote_status_candidate_name(idx);
}

void vote_status_set_candidate_name(uint8_t idx, const char *name)
{
    if (idx >= VOTE_STATUS_MAX_CANDIDATES) {
        return;
    }
    if (name == NULL || name[0] == '\0') {
        default_name(idx, s_names[idx], sizeof(s_names[idx]));
        return;
    }
    (void)snprintf(s_names[idx], sizeof(s_names[idx]), "%s", name);
}

bool vote_status_add_valid(uint8_t idx)
{
    char clk[16];

    if (idx >= vote_status_candidate_count()) {
        return false;
    }
    s_votes[idx] = (uint16_t)(s_votes[idx] + 1U);
    format_clock_hms(clk, sizeof(clk));
    vote_status_push_event(clk, "valid vote");
    (void)vote_nvs_save_votes();
    return true;
}

bool vote_status_add_spoiled(void)
{
    char clk[16];

    s_spoiled = (uint16_t)(s_spoiled + 1U);
    format_clock_hms(clk, sizeof(clk));
    vote_status_push_event(clk, "spoiled ballot");
    (void)vote_nvs_save_votes();
    return true;
}

size_t vote_status_build_json(char *out, size_t out_cap)
{
    vote_menu_settings_t *st;
    const char *phase;
    const char *phase_label;
    int countdown_sec = 0;
    char clk[16];
    char schedule_start[8];
    char schedule_end[8];
    size_t n;
    size_t off = 0U;
    uint16_t valid = 0U;
    uint8_t count;
    uint8_t i;
    const bool ds3231_ok = (BoardDs3231() != NULL);
    const bool ir_ok     = BoardPeriphReady(DEVICE_BOARD_MASK_IR);
    const bool rgb_ok      = device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED) ? true : false;
    const bool wifi_ap_ok  = net_wifi_is_started();
    const bool wifi_sta_ok = net_wifi_sta_has_ipv4();

    if (out == NULL || out_cap < 32U) {
        return 0U;
    }

    st = vote_menu_settings();
    if (st == NULL) {
        return 0U;
    }

    compute_phase(st, &phase, &phase_label, &countdown_sec);
    format_clock_hms(clk, sizeof(clk));
    (void)snprintf(schedule_start, sizeof(schedule_start), "%02u:%02u", (unsigned)st->start_h, (unsigned)st->start_m);
    (void)snprintf(schedule_end, sizeof(schedule_end), "%02u:%02u", (unsigned)st->end_h, (unsigned)st->end_m);

    count = st->candidate_count;
    if (count > VOTE_STATUS_MAX_CANDIDATES) {
        count = VOTE_STATUS_MAX_CANDIDATES;
    }
    if (count == 0U) {
        count = 1U;
    }

    valid = vote_status_valid_total();

    n = (size_t)snprintf(out + off, out_cap - off,
                         "{\"ok\":true,\"ts\":\"%s\",\"phase\":\"%s\",\"phase_label\":\"%s\","
                         "\"countdown_sec\":%d,\"cooldown_sec\":%u,"
                         "\"totals\":{\"valid\":%u,\"spoiled\":%u,\"all\":%u},"
                         "\"candidates\":[",
                         clk, phase_to_str(phase), phase_label, countdown_sec, (unsigned)st->cooldown_sec,
                         (unsigned)valid, (unsigned)s_spoiled, (unsigned)(valid + s_spoiled));
    if (n <= 0U || (off + n >= out_cap)) {
        return 0U;
    }
    off += n;

    for (i = 0U; i < count; i++) {
        n = (size_t)snprintf(out + off, out_cap - off, "%s{\"id\":%u,\"name\":\"%s\",\"votes\":%u}", (i > 0U) ? "," : "",
                             (unsigned)i, s_names[i], (unsigned)s_votes[i]);
        if (n <= 0U || (off + n >= out_cap)) {
            return 0U;
        }
        off += n;
    }

    n = (size_t)snprintf(out + off, out_cap - off,
                         "],\"schedule\":{\"start\":\"%s\",\"end\":\"%s\"},"
                         "\"peripherals\":{\"ds3231\":%s,\"ir\":%s,\"rgb\":%s,\"wifi_ap\":%s,\"wifi_sta\":%s},"
                         "\"recent_events\":[",
                         schedule_start, schedule_end, ds3231_ok ? "true" : "false", ir_ok ? "true" : "false",
                         rgb_ok ? "true" : "false", wifi_ap_ok ? "true" : "false", wifi_sta_ok ? "true" : "false");
    if (n <= 0U || (off + n >= out_cap)) {
        return 0U;
    }
    off += n;

    for (i = 0U; i < s_event_count; i++) {
        n = (size_t)snprintf(out + off, out_cap - off, "%s{\"t\":\"%s\",\"text\":\"%s\"}", (i > 0U) ? "," : "",
                             s_event_time[i], s_event_text[i]);
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
