/**
 * @file vote_menu_pages.c
 * @brief ballot_guard 管理员菜单声明式页表。
 */

#include "vote_menu_pages.h"

#include <stdio.h>

#include "vote_menu_config.h"

static menu_engine_t s_eng;
static vote_menu_settings_t s_settings = {
    .start_h          = 8U,
    .start_m          = 0U,
    .end_h            = 16U,
    .end_m            = 0U,
    .candidate_count  = 3U,
    .cooldown_sec     = 5U,
};

static vote_menu_ui_notify_cb s_ui_notify;
static void *s_ui_notify_ctx;

static const vote_menu_page_id_t s_page_admin_id   = VOTE_MENU_PAGE_ADMIN;
static const vote_menu_page_id_t s_page_time_id    = VOTE_MENU_PAGE_TIME;
static const vote_menu_page_id_t s_page_count_id   = VOTE_MENU_PAGE_COUNT;
static const vote_menu_page_id_t s_page_cooldown_id = VOTE_MENU_PAGE_COOLDOWN;
static const vote_menu_page_id_t s_page_reset_id   = VOTE_MENU_PAGE_RESET;

static void ui_toast(const char *msg, int is_error)
{
    if (s_ui_notify != NULL) {
        s_ui_notify(s_ui_notify_ctx, msg, is_error);
    }
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
        (void)st;
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
    (void)snprintf(buf, n, "%us", (unsigned)st->cooldown_sec);
    return buf;
}

static void on_save_time(void *app_ctx, menu_engine_t *eng, const menu_item_t *item)
{
    (void)item;
    if (schedule_minutes() >= schedule_end_minutes()) {
        ui_toast("Invalid time", 1);
        return;
    }
    ui_toast("Saved", 0);
    (void)menu_nav_back(eng);
    (void)app_ctx;
}

static void on_save_count(void *app_ctx, menu_engine_t *eng, const menu_item_t *item)
{
    (void)item;
    (void)app_ctx;
    ui_toast("Saved", 0);
    (void)menu_nav_back(eng);
}

static void on_save_cooldown(void *app_ctx, menu_engine_t *eng, const menu_item_t *item)
{
    (void)item;
    (void)app_ctx;
    ui_toast("Saved", 0);
    (void)menu_nav_back(eng);
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
    ui_toast("Reset OK", 0);
    while (menu_engine_depth(eng) > 0U) {
        (void)menu_nav_back(eng);
    }
}

static void on_admin_root_back(void *app_ctx, menu_engine_t *eng, const menu_page_t *page)
{
    (void)app_ctx;
    (void)eng;
    (void)page;
    ui_toast("Exit menu", 0);
}

static const menu_item_t s_time_items[] = {
    { "Start Hour", MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_hour, NULL, &s_settings.start_h },
    { "Start Min", MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_minute, NULL, &s_settings.start_m },
    { "End Hour", MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_hour, NULL, &s_settings.end_h },
    { "End Min", MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_minute, NULL, &s_settings.end_m },
    { "Save", MENU_ITEM_ACTION, 0, NULL, on_save_time, NULL, NULL, NULL },
};

static const menu_item_t s_count_items[] = {
    { "Count", MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_count, NULL, &s_settings.candidate_count },
    { "Save", MENU_ITEM_ACTION, 0, NULL, on_save_count, NULL, NULL, NULL },
};

static const menu_item_t s_cooldown_items[] = {
    { "Seconds", MENU_ITEM_PARAM, MENU_ITEM_F_ENTER_NEXT, NULL, NULL, &s_vtbl_cooldown, NULL, &s_settings.cooldown_sec },
    { "Save", MENU_ITEM_ACTION, 0, NULL, on_save_cooldown, NULL, NULL, NULL },
};

static const menu_item_t s_reset_items[] = {
    { "Cancel", MENU_ITEM_ACTION, 0, NULL, on_reset_cancel, NULL, NULL, NULL },
    { "Confirm Reset", MENU_ITEM_ACTION, MENU_ITEM_F_DANGER, NULL, on_reset_confirm, NULL, NULL, NULL },
};

static const menu_page_t s_page_time = {
    .title     = "Vote Time",
    .items     = s_time_items,
    .count     = (uint16_t)(sizeof(s_time_items) / sizeof(s_time_items[0])),
    .user_ctx  = (void *)&s_page_time_id,
    .foot_hint = "Adj Save Back",
};

static const menu_page_t s_page_count = {
    .title     = "Candidates",
    .items     = s_count_items,
    .count     = (uint16_t)(sizeof(s_count_items) / sizeof(s_count_items[0])),
    .user_ctx  = (void *)&s_page_count_id,
    .foot_hint = "Adj Save Back",
};

static const menu_page_t s_page_cooldown = {
    .title     = "Cooldown",
    .items     = s_cooldown_items,
    .count     = (uint16_t)(sizeof(s_cooldown_items) / sizeof(s_cooldown_items[0])),
    .user_ctx  = (void *)&s_page_cooldown_id,
    .foot_hint = "Adj Save Back",
};

static const menu_page_t s_page_reset = {
    .title     = "Reset",
    .items     = s_reset_items,
    .count     = (uint16_t)(sizeof(s_reset_items) / sizeof(s_reset_items[0])),
    .user_ctx  = (void *)&s_page_reset_id,
    .foot_hint = "Sel OK Back",
};

static const menu_item_t s_admin_items[] = {
    { "1.Vote Time", MENU_ITEM_SUBMENU, 0, &s_page_time, NULL, NULL, aux_schedule, NULL },
    { "2.Candidates", MENU_ITEM_SUBMENU, 0, &s_page_count, NULL, NULL, aux_count, NULL },
    { "3.Cooldown", MENU_ITEM_SUBMENU, 0, &s_page_cooldown, NULL, NULL, aux_cooldown, NULL },
    { "4.Reset Data", MENU_ITEM_SUBMENU, MENU_ITEM_F_DANGER, &s_page_reset, NULL, NULL, NULL, NULL },
};

static const menu_page_t s_page_admin = {
    .title        = "Admin",
    .items        = s_admin_items,
    .count        = (uint16_t)(sizeof(s_admin_items) / sizeof(s_admin_items[0])),
    .user_ctx     = (void *)&s_page_admin_id,
    .foot_hint    = "Sel OK Back",
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

void vote_menu_set_ui_notify(vote_menu_ui_notify_cb cb, void *ctx)
{
    s_ui_notify      = cb;
    s_ui_notify_ctx  = ctx;
}

void vote_menu_pages_init(void)
{
    menu_engine_opts_t opts = {
        .wrap_around       = 1U,
        .param_on_vertical = 1U,
    };

    menu_engine_init(&s_eng, &s_page_admin, &s_settings);
    menu_engine_configure(&s_eng, &opts);
}
