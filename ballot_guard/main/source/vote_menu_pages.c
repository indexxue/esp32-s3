/**
 * @file vote_menu_pages.c
 * @brief ballot_guard 管理员菜单声明式页表。
 */

#include "vote_menu_pages.h"

#include <stdio.h>
#include <string.h>

#include "vote_menu_config.h"
#include "vote_menu_demo.h"
#include "vote_nvs.h"
#include "vote_history.h"
#include "vote_status.h"
#include "vote_menu_zh.h"
#include "board.h"
#include "ds3231.h"

static menu_engine_t s_eng;
static vote_menu_settings_t s_settings = {
    .start_h          = 8U,
    .start_m          = 0U,
    .end_h            = 16U,
    .end_m            = 0U,
    .candidate_count  = 3U,
    .cooldown_sec     = 5U,
};

static vote_menu_clock_t s_clock = {
    .hour   = 8U,
    .minute = 0U,
    .second = 0U,
};

static vote_menu_ui_notify_cb s_ui_notify;
static void *s_ui_notify_ctx;
static vote_menu_leave_app_cb s_leave_app;
static void *s_leave_app_ctx;
static vote_menu_reset_complete_cb s_reset_complete;
static void *s_reset_complete_ctx;

static void leave_to_app(void)
{
    if (s_leave_app != NULL) {
        s_leave_app(s_leave_app_ctx);
    }
}

static const vote_menu_page_id_t s_page_admin_id    = VOTE_MENU_PAGE_ADMIN;
static const vote_menu_page_id_t s_page_time_id     = VOTE_MENU_PAGE_TIME;
static const vote_menu_page_id_t s_page_count_id    = VOTE_MENU_PAGE_COUNT;
static const vote_menu_page_id_t s_page_cooldown_id = VOTE_MENU_PAGE_COOLDOWN;
static const vote_menu_page_id_t s_page_reset_id    = VOTE_MENU_PAGE_RESET;

static void ui_toast(const char *msg, int is_error)
{
    if (s_ui_notify != NULL) {
        s_ui_notify(s_ui_notify_ctx, msg, is_error);
    }
}

static vote_nvs_cfg_t settings_to_nvs_cfg(void)
{
    vote_nvs_cfg_t cfg = {
        .start_h         = s_settings.start_h,
        .start_m         = s_settings.start_m,
        .end_h           = s_settings.end_h,
        .end_m           = s_settings.end_m,
        .candidate_count = s_settings.candidate_count,
        .cooldown_sec    = s_settings.cooldown_sec,
    };
    return cfg;
}

static int schedule_minutes(void)
{
    return (int)s_settings.start_h * 60 + (int)s_settings.start_m;
}

static int schedule_end_minutes(void)
{
    return (int)s_settings.end_h * 60 + (int)s_settings.end_m;
}

static int32_t param_get_u8(void *ctx, const menu_item_t *item)
{
    vote_menu_settings_t *st = (vote_menu_settings_t *)ctx;
    const uint8_t *field     = (const uint8_t *)item->user_ctx;

    if (st == NULL || field == NULL) {
        return 0;
    }
    return (int32_t)*field;
}

static void param_set_u8(void *ctx, const menu_item_t *item, int32_t value)
{
    vote_menu_settings_t *st = (vote_menu_settings_t *)ctx;
    uint8_t *field           = (uint8_t *)item->user_ctx;

    if (st == NULL || field == NULL) {
        return;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > 255) {
        value = 255;
    }
    *field = (uint8_t)value;
}

static int32_t step_hour(void *ctx, const menu_item_t *item, int32_t cur, int dir)
{
    (void)ctx;
    (void)item;
    int32_t next = cur + dir;
    while (next < 0) {
        next += 24;
    }
    while (next >= 24) {
        next -= 24;
    }
    return next;
}

static int32_t step_minute(void *ctx, const menu_item_t *item, int32_t cur, int dir)
{
    (void)ctx;
    (void)item;
    int32_t next = cur + dir;
    while (next < 0) {
        next += 60;
    }
    while (next >= 60) {
        next -= 60;
    }
    return next;
}

static int32_t step_count(void *ctx, const menu_item_t *item, int32_t cur, int dir)
{
    (void)ctx;
    (void)item;
    int32_t next = cur + dir;
    if (next < 2) {
        return 2;
    }
    if (next > 6) {
        return 6;
    }
    return next;
}

static int32_t step_cooldown(void *ctx, const menu_item_t *item, int32_t cur, int dir)
{
    (void)ctx;
    (void)item;
    int32_t next = cur + dir;
    if (next < 3) {
        return 3;
    }
    if (next > 10) {
        return 10;
    }
    return next;
}

static const char *fmt_hour(void *ctx, const menu_item_t *item, int32_t value, char *buf, size_t n)
{
    (void)ctx;
    (void)item;
    (void)snprintf(buf, n, "%02ld", (long)value);
    return buf;
}

static const char *fmt_minute(void *ctx, const menu_item_t *item, int32_t value, char *buf, size_t n)
{
    return fmt_hour(ctx, item, value, buf, n);
}

static const char *fmt_count(void *ctx, const menu_item_t *item, int32_t value, char *buf, size_t n)
{
    (void)ctx;
    (void)item;
    (void)snprintf(buf, n, "%ld", (long)value);
    return buf;
}

static const char *fmt_cooldown(void *ctx, const menu_item_t *item, int32_t value, char *buf, size_t n)
{
    (void)ctx;
    (void)item;
    (void)snprintf(buf, n, "%lds", (long)value);
    return buf;
}

static const menu_param_vtbl_t s_vtbl_hour = {
    .get    = param_get_u8,
    .set    = param_set_u8,
    .step   = step_hour,
    .format = fmt_hour,
};

static const menu_param_vtbl_t s_vtbl_minute = {
    .get    = param_get_u8,
    .set    = param_set_u8,
    .step   = step_minute,
    .format = fmt_minute,
};

static const menu_param_vtbl_t s_vtbl_count = {
    .get    = param_get_u8,
    .set    = param_set_u8,
    .step   = step_count,
    .format = fmt_count,
};

static const menu_param_vtbl_t s_vtbl_cooldown = {
    .get    = param_get_u8,
    .set    = param_set_u8,
    .step   = step_cooldown,
    .format = fmt_cooldown,
};

static const char *aux_schedule(void *ctx, const menu_item_t *item, char *buf, size_t n)
{
    vote_menu_settings_t *st = (vote_menu_settings_t *)ctx;
    (void)item;
    (void)snprintf(buf,
                   n,
                   "%02u:%02u-%02u:%02u",
                   (unsigned)st->start_h,
                   (unsigned)st->start_m,
                   (unsigned)st->end_h,
                   (unsigned)st->end_m);
    return buf;
}

static const char *aux_count(void *ctx, const menu_item_t *item, char *buf, size_t n)
{
    vote_menu_settings_t *st = (vote_menu_settings_t *)ctx;
    (void)item;
    (void)snprintf(buf, n, "%u", (unsigned)st->candidate_count);
    return buf;
}

static const char *aux_cooldown(void *ctx, const menu_item_t *item, char *buf, size_t n)
{
    vote_menu_settings_t *st = (vote_menu_settings_t *)ctx;
    (void)item;
    (void)snprintf(buf, n, "%u%s", (unsigned)st->cooldown_sec, VOTE_ZH_SECOND);
    return buf;
}

static void save_time_and_back(menu_engine_t *eng)
{
    vote_nvs_cfg_t cfg;

    if (schedule_minutes() >= schedule_end_minutes()) {
        ui_toast(VOTE_ZH_ERR_TIME, 1);
        return;
    }
    cfg = settings_to_nvs_cfg();
    if (!vote_nvs_save_cfg(&cfg)) {
        ui_toast(VOTE_ZH_ERR_SAVE, 1);
        return;
    }
    ui_toast(VOTE_ZH_OK_SAVED, 0);
    (void)menu_nav_back(eng);
}

static void save_count_and_back(menu_engine_t *eng)
{
    const uint8_t old_count = vote_status_candidate_count();
    vote_nvs_cfg_t cfg;

    if (vote_status_has_any_votes() && s_settings.candidate_count < old_count) {
        ui_toast(VOTE_ZH_ERR_RESET_FIRST, 1);
        return;
    }
    cfg = settings_to_nvs_cfg();
    if (!vote_nvs_save_cfg(&cfg)) {
        ui_toast(VOTE_ZH_ERR_SAVE, 1);
        return;
    }
    ui_toast(VOTE_ZH_OK_SAVED, 0);
    (void)menu_nav_back(eng);
}

static void save_cooldown_and_back(menu_engine_t *eng)
{
    vote_nvs_cfg_t cfg;

    cfg = settings_to_nvs_cfg();
    if (!vote_nvs_save_cfg(&cfg)) {
        ui_toast(VOTE_ZH_ERR_SAVE, 1);
        return;
    }
    ui_toast(VOTE_ZH_OK_SAVED, 0);
    (void)menu_nav_back(eng);
}

static bool persist_time_cfg_silent(void)
{
    vote_nvs_cfg_t cfg;

    if (schedule_minutes() >= schedule_end_minutes()) {
        return false;
    }
    cfg = settings_to_nvs_cfg();
    return vote_nvs_save_cfg(&cfg);
}

static bool persist_count_cfg_silent(void)
{
    const uint8_t old_count = vote_status_candidate_count();
    vote_nvs_cfg_t cfg;

    if (vote_status_has_any_votes() && s_settings.candidate_count < old_count) {
        return false;
    }
    cfg = settings_to_nvs_cfg();
    return vote_nvs_save_cfg(&cfg);
}

static bool persist_cooldown_cfg_silent(void)
{
    vote_nvs_cfg_t cfg;

    cfg = settings_to_nvs_cfg();
    return vote_nvs_save_cfg(&cfg);
}

static void on_time_page_exit(void *app_ctx, menu_engine_t *eng, const menu_page_t *page)
{
    (void)app_ctx;
    (void)eng;
    (void)page;
    if (!persist_time_cfg_silent()) {
        (void)vote_nvs_reload_settings();
    }
}

static void on_count_page_exit(void *app_ctx, menu_engine_t *eng, const menu_page_t *page)
{
    (void)app_ctx;
    (void)eng;
    (void)page;
    if (!persist_count_cfg_silent()) {
        (void)vote_nvs_reload_settings();
    }
}

static void on_cooldown_page_exit(void *app_ctx, menu_engine_t *eng, const menu_page_t *page)
{
    (void)app_ctx;
    (void)eng;
    (void)page;
    if (!persist_cooldown_cfg_silent()) {
        (void)vote_nvs_reload_settings();
    }
}

static void on_save_time(void *app_ctx, menu_engine_t *eng, const menu_item_t *item)
{
    (void)app_ctx;
    (void)item;
    save_time_and_back(eng);
}

static void on_save_count(void *app_ctx, menu_engine_t *eng, const menu_item_t *item)
{
    (void)app_ctx;
    (void)item;
    save_count_and_back(eng);
}

static void on_save_cooldown(void *app_ctx, menu_engine_t *eng, const menu_item_t *item)
{
    (void)app_ctx;
    (void)item;
    save_cooldown_and_back(eng);
}

bool vote_menu_pages_confirm_save(menu_engine_t *eng)
{
    const menu_page_t *page;
    vote_menu_page_id_t pid;
    uint16_t idx;

    if (eng == NULL) {
        return false;
    }

    page = menu_current_page(eng);
    pid  = vote_menu_page_id_of(page);
    idx  = menu_current_index(eng);

    switch (pid) {
    case VOTE_MENU_PAGE_TIME:
        if (idx != 4U) {
            return false;
        }
        save_time_and_back(eng);
        return true;
    case VOTE_MENU_PAGE_COUNT:
        if (idx != 1U) {
            return false;
        }
        save_count_and_back(eng);
        return true;
    case VOTE_MENU_PAGE_COOLDOWN:
        if (idx != 1U) {
            return false;
        }
        save_cooldown_and_back(eng);
        return true;
    default:
        return false;
    }
}

static void on_reset_cancel(void *app_ctx, menu_engine_t *eng, const menu_item_t *item)
{
    (void)app_ctx;
    (void)item;
    (void)menu_nav_back(eng);
}

static void on_reset_confirm(void *app_ctx, menu_engine_t *eng, const menu_item_t *item)
{
    (void)app_ctx;
    (void)item;
    (void)vote_history_archive_session_if_needed();
    vote_status_reset_counts();
    vote_status_on_vote_reset();
    ui_toast(VOTE_ZH_OK_RESET, 0);
    while (menu_engine_depth(eng) > 0U) {
        (void)menu_nav_back(eng);
    }
    if (s_reset_complete != NULL) {
        s_reset_complete(s_reset_complete_ctx);
    }
}

static void on_admin_root_back(void *app_ctx, menu_engine_t *eng, const menu_page_t *page)
{
    (void)app_ctx;
    (void)eng;
    (void)page;
    leave_to_app();
}

static void load_clock_from_rtc(void)
{
    ds3231_t *rtc = BoardDs3231();
    ds3231_datetime_t dt;

    if (rtc != NULL && ds3231_read_datetime(rtc, &dt) == DS3231_OK) {
        s_clock.hour   = dt.hour;
        s_clock.minute = dt.minute;
        s_clock.second = dt.second;
    }
}

static const menu_item_t s_time_items[] = {
    { VOTE_ZH_START_H, MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_hour, NULL, &s_settings.start_h },
    { VOTE_ZH_START_M, MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_minute, NULL, &s_settings.start_m },
    { VOTE_ZH_END_H, MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_hour, NULL, &s_settings.end_h },
    { VOTE_ZH_END_M, MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_minute, NULL, &s_settings.end_m },
    { VOTE_ZH_SAVE, MENU_ITEM_ACTION, 0, NULL, on_save_time, NULL, NULL, NULL },
};

static const menu_item_t s_count_items[] = {
    { VOTE_ZH_COUNT, MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_count, NULL, &s_settings.candidate_count },
    { VOTE_ZH_SAVE, MENU_ITEM_ACTION, 0, NULL, on_save_count, NULL, NULL, NULL },
};

static const menu_item_t s_cooldown_items[] = {
    { VOTE_ZH_SECOND, MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_cooldown, NULL, &s_settings.cooldown_sec },
    { VOTE_ZH_SAVE, MENU_ITEM_ACTION, 0, NULL, on_save_cooldown, NULL, NULL, NULL },
};

static const menu_item_t s_reset_items[] = {
    { VOTE_ZH_CANCEL, MENU_ITEM_ACTION, 0, NULL, on_reset_cancel, NULL, NULL, NULL },
    { VOTE_ZH_CONFIRM_RESET, MENU_ITEM_ACTION, MENU_ITEM_F_DANGER, NULL, on_reset_confirm, NULL, NULL, NULL },
};

static const menu_page_t s_page_time = {
    .title     = VOTE_ZH_PAGE_SCHEDULE,
    .items     = s_time_items,
    .count     = (uint16_t)(sizeof(s_time_items) / sizeof(s_time_items[0])),
    .user_ctx  = (void *)&s_page_time_id,
    .foot_hint = VOTE_ZH_FOOT_ADJ,
    .on_exit   = on_time_page_exit,
};

static const menu_page_t s_page_count = {
    .title     = VOTE_ZH_PAGE_COUNT,
    .items     = s_count_items,
    .count     = (uint16_t)(sizeof(s_count_items) / sizeof(s_count_items[0])),
    .user_ctx  = (void *)&s_page_count_id,
    .foot_hint = VOTE_ZH_FOOT_ADJ,
    .on_exit   = on_count_page_exit,
};

static const menu_page_t s_page_cooldown = {
    .title     = VOTE_ZH_PAGE_COOLDOWN,
    .items     = s_cooldown_items,
    .count     = (uint16_t)(sizeof(s_cooldown_items) / sizeof(s_cooldown_items[0])),
    .user_ctx  = (void *)&s_page_cooldown_id,
    .foot_hint = VOTE_ZH_FOOT_ADJ,
    .on_exit   = on_cooldown_page_exit,
};

static const menu_page_t s_page_reset = {
    .title     = VOTE_ZH_PAGE_RESET,
    .items     = s_reset_items,
    .count     = (uint16_t)(sizeof(s_reset_items) / sizeof(s_reset_items[0])),
    .user_ctx  = (void *)&s_page_reset_id,
    .foot_hint = VOTE_ZH_FOOT_SEL,
};

static const menu_item_t s_admin_items[] = {
    { VOTE_ZH_ADMIN_SCHEDULE, MENU_ITEM_SUBMENU, 0, &s_page_time, NULL, NULL, aux_schedule, NULL },
    { VOTE_ZH_ADMIN_COUNT, MENU_ITEM_SUBMENU, 0, &s_page_count, NULL, NULL, aux_count, NULL },
    { VOTE_ZH_ADMIN_COOLDOWN, MENU_ITEM_SUBMENU, 0, &s_page_cooldown, NULL, NULL, aux_cooldown, NULL },
    { VOTE_ZH_ADMIN_RESET, MENU_ITEM_SUBMENU, MENU_ITEM_F_DANGER, &s_page_reset, NULL, NULL, NULL, NULL },
};

static const menu_page_t s_page_admin = {
    .title        = VOTE_ZH_ADMIN_TITLE,
    .items        = s_admin_items,
    .count        = (uint16_t)(sizeof(s_admin_items) / sizeof(s_admin_items[0])),
    .user_ctx     = (void *)&s_page_admin_id,
    .foot_hint    = VOTE_ZH_FOOT_SEL,
    .on_root_back = on_admin_root_back,
};

const menu_page_t *vote_menu_root_page(void)
{
    return &s_page_admin;
}

menu_engine_t *vote_menu_engine(void)
{
    return &s_eng;
}

vote_menu_settings_t *vote_menu_settings(void)
{
    return &s_settings;
}

vote_menu_clock_t *vote_menu_clock_settings(void)
{
    return &s_clock;
}

void vote_menu_set_ui_notify(vote_menu_ui_notify_cb cb, void *ctx)
{
    s_ui_notify     = cb;
    s_ui_notify_ctx = ctx;
}

void vote_menu_set_leave_app_cb(vote_menu_leave_app_cb cb, void *ctx)
{
    s_leave_app     = cb;
    s_leave_app_ctx = ctx;
}

void vote_menu_set_reset_complete_cb(vote_menu_reset_complete_cb cb, void *ctx)
{
    s_reset_complete     = cb;
    s_reset_complete_ctx = ctx;
}

void vote_menu_pages_init(void)
{
    menu_engine_opts_t opts = {
        .wrap_around       = 0U,
        .param_on_vertical = 1U,
    };

    vote_nvs_init();
    menu_engine_init(&s_eng, &s_page_admin, &s_settings);
    menu_engine_configure(&s_eng, &opts);
}

const menu_page_t *vote_menu_page_by_id(vote_menu_page_id_t id)
{
    switch (id) {
    case VOTE_MENU_PAGE_ADMIN:
        return &s_page_admin;
    case VOTE_MENU_PAGE_TIME:
        return &s_page_time;
    case VOTE_MENU_PAGE_COUNT:
        return &s_page_count;
    case VOTE_MENU_PAGE_COOLDOWN:
        return &s_page_cooldown;
    case VOTE_MENU_PAGE_RESET:
        return &s_page_reset;
    default:
        return NULL;
    }
}

vote_menu_page_id_t vote_menu_page_id_of(const menu_page_t *page)
{
    if (page == NULL || page->user_ctx == NULL) {
        return VOTE_MENU_PAGE_ADMIN;
    }
    return *(const vote_menu_page_id_t *)page->user_ctx;
}

bool vote_menu_web_read_clock(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    load_clock_from_rtc();
    if (hour != NULL) {
        *hour = s_clock.hour;
    }
    if (minute != NULL) {
        *minute = s_clock.minute;
    }
    if (second != NULL) {
        *second = s_clock.second;
    }
    return true;
}

vote_menu_web_err_e vote_menu_web_save_schedule(uint8_t start_h, uint8_t start_m, uint8_t end_h, uint8_t end_m)
{
    vote_nvs_cfg_t cfg;

    if (start_h >= 24U || end_h >= 24U || start_m >= 60U || end_m >= 60U) {
        return VOTE_MENU_WEB_ERR_INVALID_ARG;
    }

    s_settings.start_h = start_h;
    s_settings.start_m = start_m;
    s_settings.end_h   = end_h;
    s_settings.end_m   = end_m;

    if (schedule_minutes() >= schedule_end_minutes()) {
        (void)vote_nvs_reload_settings();
        return VOTE_MENU_WEB_ERR_INVALID_TIME;
    }

    cfg = settings_to_nvs_cfg();
    if (!vote_nvs_save_cfg(&cfg)) {
        (void)vote_nvs_reload_settings();
        return VOTE_MENU_WEB_ERR_SAVE;
    }
    return VOTE_MENU_WEB_OK;
}

vote_menu_web_err_e vote_menu_web_save_count(uint8_t count)
{
    const uint8_t old_count = vote_status_candidate_count();
    vote_nvs_cfg_t cfg;

    if (count < 2U || count > 6U) {
        return VOTE_MENU_WEB_ERR_INVALID_ARG;
    }
    if (vote_status_has_any_votes() && count < old_count) {
        return VOTE_MENU_WEB_ERR_COUNT_REDUCE;
    }

    s_settings.candidate_count = count;
    cfg                        = settings_to_nvs_cfg();
    if (!vote_nvs_save_cfg(&cfg)) {
        (void)vote_nvs_reload_settings();
        return VOTE_MENU_WEB_ERR_SAVE;
    }
    return VOTE_MENU_WEB_OK;
}

vote_menu_web_err_e vote_menu_web_save_cooldown(uint8_t sec)
{
    vote_nvs_cfg_t cfg;

    if (sec < 3U || sec > 10U) {
        return VOTE_MENU_WEB_ERR_INVALID_ARG;
    }

    s_settings.cooldown_sec = sec;
    cfg                     = settings_to_nvs_cfg();
    if (!vote_nvs_save_cfg(&cfg)) {
        (void)vote_nvs_reload_settings();
        return VOTE_MENU_WEB_ERR_SAVE;
    }
    return VOTE_MENU_WEB_OK;
}

vote_menu_web_err_e vote_menu_web_save_clock(uint8_t hour, uint8_t minute, uint8_t second)
{
    if (hour >= 24U || minute >= 60U || second >= 60U) {
        return VOTE_MENU_WEB_ERR_INVALID_ARG;
    }

    s_clock.hour   = hour;
    s_clock.minute = minute;
    s_clock.second = second;
    if (!vote_status_set_clock_hms(hour, minute, second)) {
        return VOTE_MENU_WEB_ERR_CLOCK;
    }
    return VOTE_MENU_WEB_OK;
}

vote_menu_web_err_e vote_menu_web_reset_data(void)
{
    (void)vote_history_archive_session_if_needed();
    vote_status_reset_counts();
    vote_status_on_vote_reset();
    vote_menu_demo_on_data_reset();
    return VOTE_MENU_WEB_OK;
}
