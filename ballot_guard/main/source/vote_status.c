/**
 * @file vote_status.c
 * @brief 投票看板 JSON 快照：时段、票数、外设状态（与 LCD 状态机逐步对齐）。
 */

#include "vote_status.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "device_profile.h"
#include "esp_timer.h"
#include "net_wifi.h"
#include "vote_menu_pages.h"

#define VOTE_STATUS_JSON_MAX (2048U)

static const char *s_default_names[VOTE_STATUS_MAX_CANDIDATES] = {
    "张三", "李四", "王五", "赵六", "孙七", "周八",
};

static const char *s_lcd_names[VOTE_STATUS_MAX_CANDIDATES] = {
    "A", "B", "C", "D", "E", "F",
};

static uint16_t s_votes[VOTE_STATUS_MAX_CANDIDATES];
static uint16_t s_spoiled;

static char s_event_text[VOTE_STATUS_MAX_EVENTS][48];
static char s_event_time[VOTE_STATUS_MAX_EVENTS][12];
static uint8_t s_event_count;

static int now_minutes_of_day(void)
{
    const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000LL);
    const uint32_t h   = (sec / 3600U) % 24U;
    const uint32_t m   = (sec / 60U) % 60U;

    return (int)(h * 60U + m);
}

static void format_clock_hms(char *buf, size_t cap)
{
    const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000LL);
    const uint32_t h   = (sec / 3600U) % 24U;
    const uint32_t m   = (sec / 60U) % 60U;
    const uint32_t s   = sec % 60U;

    if (buf == NULL || cap == 0U) {
        return;
    }
    (void)snprintf(buf, cap, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
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
    const int start_min = (int)st->start_h * 60 + (int)st->start_m;
    const int end_min   = (int)st->end_h * 60 + (int)st->end_m;
    const int now_min   = now_minutes_of_day();

    if (out_countdown_sec != NULL) {
        *out_countdown_sec = 0;
    }

    if (start_min >= end_min) {
        *out_phase  = "idle";
        *out_label  = phase_label_of("idle");
        return;
    }

    if (now_min < start_min) {
        *out_phase = "waiting";
        *out_label = phase_label_of("waiting");
        if (out_countdown_sec != NULL) {
            *out_countdown_sec = (start_min - now_min) * 60;
        }
        return;
    }

    if (now_min >= end_min) {
        *out_phase = "locked";
        *out_label = phase_label_of("locked");
        return;
    }

    *out_phase = "voting";
    *out_label = phase_label_of("voting");
    if (out_countdown_sec != NULL) {
        *out_countdown_sec = (end_min - now_min) * 60;
    }
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

const char *vote_status_candidate_name(uint8_t idx)
{
    if (idx >= VOTE_STATUS_MAX_CANDIDATES) {
        return "?";
    }
    return s_default_names[idx];
}

const char *vote_status_candidate_lcd_name(uint8_t idx)
{
    if (idx >= VOTE_STATUS_MAX_CANDIDATES) {
        return "?";
    }
    return s_lcd_names[idx];
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
    const bool ds3231_ok = false;
    const bool ir_ok       = true;
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

    for (i = 0U; i < count; i++) {
        valid = (uint16_t)(valid + s_votes[i]);
    }

    n = (size_t)snprintf(out + off, out_cap - off,
                         "{\"ok\":true,\"ts\":\"%s\",\"phase\":\"%s\",\"phase_label\":\"%s\","
                         "\"countdown_sec\":%d,\"cooldown_sec\":0,"
                         "\"totals\":{\"valid\":%u,\"spoiled\":%u,\"all\":%u},"
                         "\"candidates\":[",
                         clk, phase_to_str(phase), phase_label, countdown_sec, (unsigned)valid, (unsigned)s_spoiled,
                         (unsigned)(valid + s_spoiled));
    if (n <= 0U || (off + n >= out_cap)) {
        return 0U;
    }
    off += n;

    for (i = 0U; i < count; i++) {
        n = (size_t)snprintf(out + off, out_cap - off, "%s{\"id\":%u,\"name\":\"%s\",\"votes\":%u}", (i > 0U) ? "," : "",
                             (unsigned)i, s_default_names[i], (unsigned)s_votes[i]);
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
