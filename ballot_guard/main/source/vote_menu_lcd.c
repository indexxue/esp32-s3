/**
 * @file vote_menu_lcd.c
 * @brief ballot_guard 菜单 LCD 视图（240x135 横屏，对齐菜单 UI 设计稿）。
 */

#include "vote_menu_lcd.h"

#include <stdio.h>
#include <string.h>

#include "device_profile.h"
#include "lcd.h"
#include "net_wifi.h"
#include "vote_menu_config.h"
#include "vote_menu_pages.h"
#include "vote_status.h"
#include "vote_history.h"
#include "lcd_font.h"
#include "vote_menu_zh.h"

#define VOTE_MENU_FONT_SM (12U)
#define VOTE_MENU_FONT_MD (16U)
#define VOTE_MENU_LIST_ROW_H (20U)

static vote_menu_page_id_t page_id_of(const menu_page_t *page)
{
    if (page == NULL || page->user_ctx == NULL) {
        return VOTE_MENU_PAGE_ADMIN;
    }
    return *(const vote_menu_page_id_t *)page->user_ctx;
}

static uint16_t text_width(const char *s, uint8_t sizey)
{
    return lcd_font_text_width(s, sizey);
}

static uint16_t title_clock_end_x(const char *clk)
{
    return (uint16_t)(VOTE_MENU_TITLE_CLK_X + text_width(clk, VOTE_MENU_FONT_SM) + VOTE_MENU_TITLE_CLK_GAP);
}

static void draw_string(st7789_t *lcd, uint16_t x, uint16_t y, const char *s, uint16_t fc, uint16_t bc, uint8_t sizey)
{
    if (lcd == NULL || s == NULL) {
        return;
    }
    lcd_font_draw_text(lcd, x, y, s, fc, bc, sizey, 0U);
}

static void draw_string_center(st7789_t *lcd, uint16_t y, const char *s, uint16_t fc, uint16_t bc, uint8_t sizey)
{
    uint16_t w;
    uint16_t x;

    w = text_width(s, sizey);
    x = (w >= VOTE_MENU_LCD_W) ? 0U : (uint16_t)((VOTE_MENU_LCD_W - w) / 2U);
    draw_string(lcd, x, y, s, fc, bc, sizey);
}

static void format_clock_text(char *clk, size_t clk_len)
{
    (void)vote_status_format_clock(clk, clk_len);
}

static uint16_t title_bar_title_x(const char *clk, const char *title)
{
    uint16_t title_w;
    uint16_t left;
    uint16_t right;
    uint16_t avail;

    title_w = text_width(title, VOTE_MENU_FONT_SM);
    left    = title_clock_end_x(clk);
    right   = (uint16_t)(VOTE_MENU_LCD_W - VOTE_MENU_TITLE_IP_W);

    if (right <= left || title_w == 0U) {
        return (title_w >= VOTE_MENU_LCD_W) ? 0U : (uint16_t)((VOTE_MENU_LCD_W - title_w) / 2U);
    }

    avail = (uint16_t)(right - left);
    if (title_w >= avail) {
        return left;
    }
    return (uint16_t)(left + (avail - title_w) / 2U);
}

static void draw_title_ip(st7789_t *lcd)
{
    char ip[20];
    uint16_t w;
    uint16_t x;
    const bool ip_ok = net_wifi_format_ipv4_for_display(ip, sizeof(ip));
    const uint16_t fc = ip_ok ? VOTE_MENU_COLOR_TEXT : VOTE_MENU_COLOR_MUTED;

    if (lcd == NULL) {
        return;
    }

    if (!ip_ok) {
        (void)snprintf(ip, sizeof(ip), "---");
    }

    w = text_width(ip, VOTE_MENU_FONT_SM);
    x = (w >= (VOTE_MENU_LCD_W - 8U)) ? 0U : (uint16_t)(VOTE_MENU_LCD_W - w - 4U);
    lcd_fill(lcd, (uint16_t)(VOTE_MENU_LCD_W - VOTE_MENU_TITLE_IP_W), 4U, VOTE_MENU_LCD_W, 16U,
             VOTE_MENU_COLOR_SURFACE);
    draw_string(lcd, x, 4U, ip, fc, VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
}

static void draw_title_bar(st7789_t *lcd, const menu_page_t *page)
{
    char clk[16];
    const char *title;
    uint16_t title_x;

    lcd_fill(lcd, 0U, 0U, VOTE_MENU_LCD_W, VOTE_MENU_TITLE_H, VOTE_MENU_COLOR_SURFACE);
    lcd_draw_line(lcd, 0U, (uint16_t)(VOTE_MENU_TITLE_H - 1U), VOTE_MENU_LCD_W, (uint16_t)(VOTE_MENU_TITLE_H - 1U),
                  VOTE_MENU_COLOR_BORDER);

    format_clock_text(clk, sizeof(clk));
    draw_string(lcd, VOTE_MENU_TITLE_CLK_X, 4U, clk, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE,
                VOTE_MENU_FONT_SM);

    title = (page != NULL && page->title != NULL) ? page->title : VOTE_ZH_ADMIN_TITLE;
    title_x = title_bar_title_x(clk, title);
    draw_string(lcd, title_x, 4U, title, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
    draw_title_ip(lcd);
}

static void draw_foot(st7789_t *lcd, const menu_page_t *page)
{
    const char *hint = menu_page_foot_hint(page);

    lcd_fill(lcd, 0U, VOTE_MENU_FOOT_Y, VOTE_MENU_LCD_W, VOTE_MENU_LCD_H, VOTE_MENU_COLOR_SURFACE);
    lcd_draw_line(lcd, 0U, VOTE_MENU_FOOT_Y, VOTE_MENU_LCD_W, VOTE_MENU_FOOT_Y, VOTE_MENU_COLOR_BORDER);
    if (hint[0] != '\0') {
        draw_string_center(lcd, (uint16_t)(VOTE_MENU_FOOT_Y + 2U), hint, VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_SURFACE,
                           VOTE_MENU_FONT_SM);
    }
}

static void draw_list_row(st7789_t *lcd,
                          uint16_t y,
                          const menu_item_t *item,
                          uint16_t focus,
                          void *app_ctx,
                          char *aux_buf,
                          size_t aux_len,
                          uint16_t row_h)
{
    uint16_t bg  = VOTE_MENU_COLOR_BG;
    uint16_t fc  = VOTE_MENU_COLOR_TEXT;
    uint16_t lnc = VOTE_MENU_COLOR_BG;
    const char *label;
    char prefix[4];
    const uint16_t row_bottom = (uint16_t)(y + row_h - 1U);

    if (item == NULL) {
        return;
    }

    if (focus != 0U) {
        bg  = menu_item_is_danger(item) ? VOTE_MENU_COLOR_DANGER_BG : VOTE_MENU_COLOR_SURFACE2;
        lnc = menu_item_is_danger(item) ? VOTE_MENU_COLOR_DANGER : VOTE_MENU_COLOR_ACCENT;
        fc  = menu_item_is_danger(item) ? VOTE_MENU_COLOR_DANGER : VOTE_MENU_COLOR_TEXT;
    } else if (menu_item_is_danger(item)) {
        fc = VOTE_MENU_COLOR_DANGER;
    } else {
        fc = VOTE_MENU_COLOR_MUTED;
    }

    lcd_fill(lcd, 0U, y, VOTE_MENU_LCD_W, row_bottom, bg);
    if (lnc != VOTE_MENU_COLOR_BG) {
        lcd_fill(lcd, 0U, y, 3U, row_bottom, lnc);
    }

    prefix[0] = focus ? '>' : ' ';
    prefix[1] = ' ';
    prefix[2] = '\0';
    label     = (item->label != NULL) ? item->label : "";
    draw_string(lcd, 8U, (uint16_t)(y + 4U), prefix, fc, bg, VOTE_MENU_FONT_SM);
    draw_string(lcd, 20U, (uint16_t)(y + 4U), label, fc, bg, VOTE_MENU_FONT_SM);

    {
        const char *aux = menu_item_aux_text(app_ctx, item, aux_buf, aux_len);
        uint16_t ax;
        if (aux != NULL && aux[0] != '\0') {
            ax = (uint16_t)(VOTE_MENU_LCD_W - text_width(aux, VOTE_MENU_FONT_SM) - 6U);
            draw_string(lcd, ax, (uint16_t)(y + 4U), aux, VOTE_MENU_COLOR_MUTED, bg, VOTE_MENU_FONT_SM);
        }
    }
}

static void draw_simple_scrollbar(st7789_t *lcd, uint16_t track_y, uint16_t track_h, uint8_t total, uint8_t visible, uint8_t focus_idx)
{
    uint16_t thumb_h;
    uint16_t thumb_y;

    if (lcd == NULL || total <= visible) {
        return;
    }
    if (focus_idx >= total) {
        focus_idx = (uint8_t)(total - 1U);
    }

    thumb_h = (uint16_t)((uint32_t)track_h * visible / total);
    if (thumb_h < 6U) {
        thumb_h = 6U;
    }
    if (thumb_h > track_h) {
        thumb_h = track_h;
    }

    thumb_y = track_y;
    if (total > 1U) {
        thumb_y = (uint16_t)(track_y + ((uint32_t)focus_idx * (track_h - thumb_h) / (total - 1U)));
    }

    lcd_fill(lcd, 234U, track_y, 237U, (uint16_t)(track_y + track_h), VOTE_MENU_COLOR_BORDER);
    lcd_fill(lcd, 234U, thumb_y, 237U, (uint16_t)(thumb_y + thumb_h), VOTE_MENU_COLOR_ACCENT);
}

static void draw_list_page(st7789_t *lcd, const menu_engine_t *eng, const menu_page_t *page)
{
    char aux_buf[32];
    const uint8_t visible = (uint8_t)VOTE_MENU_VISIBLE_ROWS;
    const uint16_t row_h  = VOTE_MENU_LIST_ROW_H;
    const uint16_t list_y0 = (uint16_t)(VOTE_MENU_BODY_Y + 4U);
    const uint16_t track_h = (uint16_t)(visible * row_h - 2U);
    uint16_t first_slot = 0U;
    uint16_t focus_pos  = 0U;
    uint16_t focus_count;
    uint16_t cur_idx;
    uint16_t i;

    if (lcd == NULL || eng == NULL || page == NULL) {
        return;
    }

    cur_idx     = menu_current_index(eng);
    focus_count = menu_page_focus_item_count(page);
    (void)menu_page_viewport_first_focus(page, cur_idx, visible, &first_slot);
    (void)menu_page_focus_cursor_pos(page, cur_idx, &focus_pos);

    for (i = 0U; i < visible; i++) {
        uint16_t item_index = 0U;
        uint16_t y          = (uint16_t)(list_y0 + i * row_h);
        const menu_item_t *item;

        if (!menu_page_focus_item_at(page, (uint16_t)(first_slot + i), &item_index)) {
            lcd_fill(lcd, 0U, y, VOTE_MENU_LCD_W, (uint16_t)(y + row_h - 1U), VOTE_MENU_COLOR_BG);
            continue;
        }
        item = &page->items[item_index];
        draw_list_row(lcd, y, item, (uint16_t)(item_index == cur_idx), eng->app_ctx, aux_buf, sizeof(aux_buf), row_h);
    }

    if (focus_count > 0U && focus_count <= 255U) {
        draw_simple_scrollbar(lcd, list_y0, track_h, (uint8_t)focus_count, visible, (uint8_t)focus_pos);
    }
}

static void draw_string_center_in_box(st7789_t *lcd,
                                      uint16_t x,
                                      uint16_t y,
                                      uint16_t bw,
                                      uint16_t bh,
                                      const char *s,
                                      uint16_t fc,
                                      uint16_t bc,
                                      uint8_t sizey)
{
    uint16_t tw = text_width(s, sizey);
    uint16_t tx = (tw >= bw) ? x : (uint16_t)(x + (bw - tw) / 2U);
    uint16_t ty = (uint16_t)(y + (bh > sizey ? (bh - sizey) / 2U : 0U));
    draw_string(lcd, tx, ty, s, fc, bc, sizey);
}

static void draw_digit_box(st7789_t *lcd, uint16_t x, uint16_t y, const char *txt, uint16_t focus)
{
    uint16_t bg = focus ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_SURFACE;
    uint16_t fc = VOTE_MENU_COLOR_TEXT;
    uint16_t bc = focus ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_SURFACE;

    lcd_fill(lcd, x, y, (uint16_t)(x + 36U), (uint16_t)(y + 28U), bg);
    lcd_draw_rectangle(lcd, x, y, (uint16_t)(x + 35U), (uint16_t)(y + 27U), VOTE_MENU_COLOR_BORDER);
    draw_string_center_in_box(lcd, x, y, 36U, 28U, txt, fc, bc, VOTE_MENU_FONT_MD);
}

static void draw_time_page(st7789_t *lcd, const menu_engine_t *eng)
{
    vote_menu_settings_t *st = vote_menu_settings();
    char buf[8];
    uint16_t idx             = menu_current_index(eng);

    draw_string(lcd, 16U, (uint16_t)(VOTE_MENU_BODY_Y + 4U), VOTE_ZH_START, VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_SM);
    (void)snprintf(buf, sizeof(buf), "%02u", (unsigned)st->start_h);
    draw_digit_box(lcd, 24U, (uint16_t)(VOTE_MENU_BODY_Y + 18U), buf, (uint16_t)(idx == 0U));
    draw_string(lcd, 64U, (uint16_t)(VOTE_MENU_BODY_Y + 24U), ":", VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_MD);
    (void)snprintf(buf, sizeof(buf), "%02u", (unsigned)st->start_m);
    draw_digit_box(lcd, 76U, (uint16_t)(VOTE_MENU_BODY_Y + 18U), buf, (uint16_t)(idx == 1U));

    draw_string(lcd, 16U, (uint16_t)(VOTE_MENU_BODY_Y + 50U), VOTE_ZH_END, VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_SM);
    (void)snprintf(buf, sizeof(buf), "%02u", (unsigned)st->end_h);
    draw_digit_box(lcd, 24U, (uint16_t)(VOTE_MENU_BODY_Y + 64U), buf, (uint16_t)(idx == 2U));
    draw_string(lcd, 64U, (uint16_t)(VOTE_MENU_BODY_Y + 70U), ":", VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_MD);
    (void)snprintf(buf, sizeof(buf), "%02u", (unsigned)st->end_m);
    draw_digit_box(lcd, 76U, (uint16_t)(VOTE_MENU_BODY_Y + 64U), buf, (uint16_t)(idx == 3U));

    {
        uint16_t bx = 80U;
        uint16_t by = (uint16_t)(VOTE_MENU_BODY_Y + 96U);
        uint16_t bg = (idx == 4U) ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_SURFACE;
        lcd_fill(lcd, bx, by, (uint16_t)(bx + 80U), (uint16_t)(by + 24U), bg);
        lcd_draw_rectangle(lcd, bx, by, (uint16_t)(bx + 79U), (uint16_t)(by + 23U), VOTE_MENU_COLOR_BORDER);
        draw_string_center_in_box(lcd, bx, by, 80U, 24U, VOTE_ZH_SAVE, VOTE_MENU_COLOR_TEXT, bg, VOTE_MENU_FONT_SM);
    }
}

static void draw_count_page(st7789_t *lcd, const menu_engine_t *eng)
{
    vote_menu_settings_t *st = vote_menu_settings();
    char buf[8];
    char names[80];
    uint16_t idx = menu_current_index(eng);
    uint8_t count;
    uint8_t i;

    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 4U), VOTE_ZH_PAGE_COUNT, VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    (void)snprintf(buf, sizeof(buf), "%u", (unsigned)st->candidate_count);
    {
        uint16_t bx = 88U;
        uint16_t by = (uint16_t)(VOTE_MENU_BODY_Y + 20U);
        uint16_t bg = (idx == 0U) ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_SURFACE;
        lcd_fill(lcd, bx, by, (uint16_t)(bx + 64U), (uint16_t)(by + 36U), bg);
        lcd_draw_rectangle(lcd, bx, by, (uint16_t)(bx + 63U), (uint16_t)(by + 35U),
                           (idx == 0U) ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_BORDER);
        draw_string_center_in_box(lcd, bx, by, 64U, 36U, buf, VOTE_MENU_COLOR_TEXT, bg, VOTE_MENU_FONT_MD);
    }
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 60U), VOTE_ZH_RANGE, VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);

    count = st->candidate_count;
    if (count > VOTE_STATUS_MAX_CANDIDATES) {
        count = VOTE_STATUS_MAX_CANDIDATES;
    }
    names[0] = '\0';
    for (i = 0U; i < count; i++) {
        const char *nm = vote_status_candidate_lcd_name(i);
        if (i > 0U) {
            (void)strncat(names, ", ", sizeof(names) - strlen(names) - 1U);
        }
        (void)strncat(names, nm, sizeof(names) - strlen(names) - 1U);
    }
    draw_string(lcd, 8U, (uint16_t)(VOTE_MENU_BODY_Y + 72U), VOTE_ZH_NAMES_WEB, VOTE_MENU_COLOR_MUTED,
                VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    draw_string(lcd, 8U, (uint16_t)(VOTE_MENU_BODY_Y + 84U), names, VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_SM);
}

static void draw_cooldown_page(st7789_t *lcd, const menu_engine_t *eng)
{
    vote_menu_settings_t *st = vote_menu_settings();
    char buf[8];
    uint16_t idx             = menu_current_index(eng);

    if (st == NULL) {
        return;
    }

    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 4U), VOTE_ZH_PAGE_COOLDOWN, VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 18U), VOTE_ZH_SECOND, VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    (void)snprintf(buf, sizeof(buf), "%u", (unsigned)st->cooldown_sec);
    {
        uint16_t bx = 88U;
        uint16_t by = (uint16_t)(VOTE_MENU_BODY_Y + 34U);
        uint16_t bg = (idx == 0U) ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_SURFACE;
        lcd_fill(lcd, bx, by, (uint16_t)(bx + 64U), (uint16_t)(by + 36U), bg);
        lcd_draw_rectangle(lcd, bx, by, (uint16_t)(bx + 63U), (uint16_t)(by + 35U),
                           (idx == 0U) ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_BORDER);
        draw_string_center_in_box(lcd, bx, by, 64U, 36U, buf, VOTE_MENU_COLOR_TEXT, bg, VOTE_MENU_FONT_MD);
    }
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 76U), "3 - 10", VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                       VOTE_MENU_FONT_SM);
}

static void draw_reset_modal(st7789_t *lcd, const menu_engine_t *eng, const menu_page_t *page)
{
    uint16_t idx = menu_current_index(eng);
    char aux_buf[32];

    lcd_fill(lcd, 0U, VOTE_MENU_BODY_Y, VOTE_MENU_LCD_W, VOTE_MENU_FOOT_Y, VOTE_MENU_COLOR_BG);
    lcd_fill(lcd, 10U, (uint16_t)(VOTE_MENU_BODY_Y + 10U), 230U, (uint16_t)(VOTE_MENU_BODY_Y + 82U),
             VOTE_MENU_COLOR_SURFACE);
    lcd_draw_rectangle(lcd, 10U, (uint16_t)(VOTE_MENU_BODY_Y + 10U), 229U, (uint16_t)(VOTE_MENU_BODY_Y + 81U),
                       VOTE_MENU_COLOR_BORDER);

    draw_string(lcd, 18U, (uint16_t)(VOTE_MENU_BODY_Y + 18U), VOTE_ZH_RESET_ASK, VOTE_MENU_COLOR_TEXT,
                VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
    draw_string(lcd, 18U, (uint16_t)(VOTE_MENU_BODY_Y + 34U), VOTE_ZH_NO_UNDO, VOTE_MENU_COLOR_MUTED,
                VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);

    if (page != NULL && page->items != NULL && page->count >= 2U) {
        uint16_t y = (uint16_t)(VOTE_MENU_BODY_Y + 54U);
        draw_list_row(lcd, y, &page->items[0], (uint16_t)(idx == 0U), eng->app_ctx, aux_buf, sizeof(aux_buf),
                      VOTE_MENU_ROW_H);
        draw_list_row(lcd, (uint16_t)(y + VOTE_MENU_ROW_H), &page->items[1], (uint16_t)(idx == 1U), eng->app_ctx,
                      aux_buf, sizeof(aux_buf), VOTE_MENU_ROW_H);
    }
}

static const char *phase_lcd_label(const char *phase)
{
    if (phase == NULL || phase[0] == '\0') {
        return VOTE_ZH_STANDBY;
    }
    if (strcmp(phase, "idle") == 0) {
        return VOTE_ZH_STANDBY;
    }
    if (strcmp(phase, "waiting") == 0) {
        return VOTE_ZH_WAITING;
    }
    if (strcmp(phase, "voting") == 0) {
        return VOTE_ZH_VOTING;
    }
    if (strcmp(phase, "selecting") == 0) {
        return VOTE_ZH_SELECT;
    }
    if (strcmp(phase, "cooldown") == 0) {
        return VOTE_ZH_COOLDOWN;
    }
    if (strcmp(phase, "violation") == 0) {
        return VOTE_ZH_VIOLATION;
    }
    if (strcmp(phase, "locked") == 0) {
        return VOTE_ZH_LOCKED;
    }
    if (strcmp(phase, "fault") == 0) {
        return VOTE_ZH_FAULT;
    }
    return VOTE_ZH_STANDBY;
}

static void draw_admin_entry_progress(st7789_t *lcd, uint8_t pct)
{
    const uint16_t cx = 208U;
    const uint16_t cy = 108U;
    const uint16_t r  = 16U;

    if (lcd == NULL || pct == 0U) {
        return;
    }

    lcd_draw_circle(lcd, cx, cy, r, VOTE_MENU_COLOR_BORDER);
    if (pct >= 100U) {
        lcd_draw_circle(lcd, cx, cy, (uint16_t)(r - 2U), VOTE_MENU_COLOR_ACCENT);
    } else if (pct >= 50U) {
        lcd_draw_circle(lcd, cx, cy, (uint16_t)(r - 4U), VOTE_MENU_COLOR_SURFACE2);
        lcd_fill(lcd, (uint16_t)(cx - 2U), (uint16_t)(cy - r + 2U), (uint16_t)(cx + 2U), (uint16_t)(cy - r + 6U),
                 VOTE_MENU_COLOR_ACCENT);
    } else {
        lcd_fill(lcd, (uint16_t)(cx - 2U), (uint16_t)(cy + r - 6U), (uint16_t)(cx + 2U), (uint16_t)(cy + r - 2U),
                 VOTE_MENU_COLOR_ACCENT);
    }
}

static void format_remain_mmss(char *buf, size_t cap, int countdown_sec)
{
    if (buf == NULL || cap == 0U) {
        return;
    }
    if (countdown_sec <= 0) {
        buf[0] = '\0';
        return;
    }
    (void)snprintf(buf, cap, VOTE_ZH_REMAIN " %02u:%02u", (unsigned)(countdown_sec / 60),
                   (unsigned)(countdown_sec % 60));
}

static void format_select_title(char *buf, size_t cap, uint8_t timeout_sec)
{
    if (buf == NULL || cap == 0U) {
        return;
    }
    if (timeout_sec > 0U) {
        (void)snprintf(buf, cap, VOTE_ZH_REMAIN " %02u" VOTE_ZH_SECOND, (unsigned)timeout_sec);
    } else {
        buf[0] = '\0';
    }
}

static void draw_title_right_label(st7789_t *lcd, const char *label)
{
    uint16_t w;
    uint16_t x;

    if (lcd == NULL || label == NULL || label[0] == '\0') {
        return;
    }

    w = text_width(label, VOTE_MENU_FONT_SM);
    x = (w >= (VOTE_MENU_LCD_W - 8U)) ? 0U : (uint16_t)(VOTE_MENU_LCD_W - w - 4U);
    lcd_fill(lcd, (uint16_t)(VOTE_MENU_LCD_W - VOTE_MENU_TITLE_IP_W), 4U, VOTE_MENU_LCD_W, 16U,
             VOTE_MENU_COLOR_SURFACE);
    draw_string(lcd, x, 4U, label, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
}

static void draw_countdown_title_bar(st7789_t *lcd, const char *title, const char *right_label)
{
    char clk[16];
    uint16_t title_x;

    lcd_fill(lcd, 0U, 0U, VOTE_MENU_LCD_W, VOTE_MENU_TITLE_H, VOTE_MENU_COLOR_SURFACE);
    lcd_draw_line(lcd, 0U, (uint16_t)(VOTE_MENU_TITLE_H - 1U), VOTE_MENU_LCD_W, (uint16_t)(VOTE_MENU_TITLE_H - 1U),
                  VOTE_MENU_COLOR_BORDER);

    format_clock_text(clk, sizeof(clk));
    draw_string(lcd, VOTE_MENU_TITLE_CLK_X, 4U, clk, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE,
                VOTE_MENU_FONT_SM);

    if (title != NULL && title[0] != '\0') {
        title_x = title_bar_title_x(clk, title);
        draw_string(lcd, title_x, 4U, title, VOTE_MENU_COLOR_WARN, VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
    }
    if (right_label != NULL && right_label[0] != '\0') {
        draw_title_right_label(lcd, right_label);
    }
}

static void draw_select_title_bar(st7789_t *lcd, uint8_t timeout_sec)
{
    char title[24];

    format_select_title(title, sizeof(title), timeout_sec);
    draw_countdown_title_bar(lcd, title, VOTE_ZH_BALLOT);
}

static void draw_waiting_title_bar(st7789_t *lcd, int countdown_sec)
{
    char title[24];

    format_remain_mmss(title, sizeof(title), countdown_sec);
    draw_countdown_title_bar(lcd, title, VOTE_ZH_WAITING);
}

static void draw_voting_title_bar(st7789_t *lcd, int countdown_sec)
{
    char title[24];

    format_remain_mmss(title, sizeof(title), countdown_sec);
    draw_countdown_title_bar(lcd, title, VOTE_ZH_VOTING);
}

static void draw_business_title(st7789_t *lcd, const char *title)
{
    char clk[16];
    uint16_t title_w;
    uint16_t title_x;

    lcd_fill(lcd, 0U, 0U, VOTE_MENU_LCD_W, VOTE_MENU_TITLE_H, VOTE_MENU_COLOR_SURFACE);
    lcd_draw_line(lcd, 0U, (uint16_t)(VOTE_MENU_TITLE_H - 1U), VOTE_MENU_LCD_W, (uint16_t)(VOTE_MENU_TITLE_H - 1U),
                  VOTE_MENU_COLOR_BORDER);

    format_clock_text(clk, sizeof(clk));
    draw_string(lcd, VOTE_MENU_TITLE_CLK_X, 4U, clk, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE,
                VOTE_MENU_FONT_SM);

    if (title == NULL || title[0] == '\0') {
        return;
    }

    title_w = text_width(title, VOTE_MENU_FONT_SM);
    title_x = (title_w + title_clock_end_x(clk) <= VOTE_MENU_LCD_W) ? (uint16_t)(VOTE_MENU_LCD_W - title_w - 4U)
                                                                    : title_clock_end_x(clk);
    draw_string(lcd, title_x, 4U, title, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
}

static const char *select_foot_hint(void)
{
    return VOTE_ZH_FOOT_SELECT;
}

static const char *phase_badge_text(void)
{
    int cd = 0;
    const char *phase = vote_status_current_phase(&cd);

    return phase_lcd_label(phase);
}

static void draw_business_foot(st7789_t *lcd, const char *hint)
{
    lcd_fill(lcd, 0U, VOTE_MENU_FOOT_Y, VOTE_MENU_LCD_W, VOTE_MENU_LCD_H, VOTE_MENU_COLOR_SURFACE);
    lcd_draw_line(lcd, 0U, VOTE_MENU_FOOT_Y, VOTE_MENU_LCD_W, VOTE_MENU_FOOT_Y, VOTE_MENU_COLOR_BORDER);
    if (hint != NULL && hint[0] != '\0') {
        draw_string_center(lcd, (uint16_t)(VOTE_MENU_FOOT_Y + 2U), hint, VOTE_MENU_COLOR_MUTED,
                           VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
    }
}

static void draw_home_screen(st7789_t *lcd, uint8_t scroll_idx, uint8_t admin_progress)
{
    char line[48];
    uint8_t count = vote_status_candidate_count();
    uint8_t i;
    uint16_t valid = 0U;
    uint16_t y     = (uint16_t)(VOTE_MENU_BODY_Y + 2U);
    uint8_t visible = 3U;
    uint8_t start;
    int countdown = 0;
    const char *phase = vote_status_current_phase(&countdown);

    if (count == 0U) {
        count = 1U;
    }
    if (scroll_idx >= count) {
        scroll_idx = 0U;
    }
    start = scroll_idx;
    if (count > visible && start + visible > count) {
        start = (uint8_t)(count - visible);
    }

    if (strcmp(phase, "waiting") == 0 && countdown > 0) {
        draw_waiting_title_bar(lcd, countdown);
    } else {
        draw_business_title(lcd, VOTE_ZH_BOARD);
        if (strcmp(phase, "waiting") != 0) {
            draw_string(lcd, 170U, 4U, phase_badge_text(), VOTE_MENU_COLOR_WARN, VOTE_MENU_COLOR_SURFACE,
                        VOTE_MENU_FONT_SM);
        }
    }

    draw_string(lcd, 8U, y, VOTE_ZH_CANDIDATE, VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    draw_string(lcd, 190U, y, VOTE_ZH_VOTES, VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    y = (uint16_t)(y + 14U);

    for (i = start; i < count && i < (uint8_t)(start + visible); i++) {
        uint16_t bg = (i == scroll_idx) ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_BG;
        uint16_t fc = VOTE_MENU_COLOR_TEXT;
        char prefix[4];
        char vbuf[8];

        valid = (uint16_t)(valid + vote_status_votes(i));
        prefix[0] = (i == scroll_idx) ? '>' : ' ';
        prefix[1] = ' ';
        prefix[2] = '\0';
        lcd_fill(lcd, 0U, y, VOTE_MENU_LCD_W, (uint16_t)(y + 18U), bg);
        if (i == scroll_idx) {
            lcd_fill(lcd, 0U, y, 3U, (uint16_t)(y + 18U), VOTE_MENU_COLOR_ACCENT);
        }
        draw_string(lcd, 8U, (uint16_t)(y + 3U), prefix, fc, bg, VOTE_MENU_FONT_SM);
        draw_string(lcd, 20U, (uint16_t)(y + 3U), vote_status_candidate_lcd_name(i), fc, bg, VOTE_MENU_FONT_SM);
        (void)snprintf(vbuf, sizeof(vbuf), "%u", (unsigned)vote_status_votes(i));
        draw_string(lcd, 200U, (uint16_t)(y + 3U), vbuf, VOTE_MENU_COLOR_MUTED, bg, VOTE_MENU_FONT_SM);
        y = (uint16_t)(y + 18U);
    }

    draw_simple_scrollbar(lcd,
                          (uint16_t)(VOTE_MENU_BODY_Y + 16U),
                          (uint16_t)(visible * 18U - 2U),
                          count,
                          visible,
                          scroll_idx);

    valid = vote_status_valid_total();
    (void)snprintf(line,
                   sizeof(line),
                   VOTE_ZH_VALID " %u  " VOTE_ZH_SPOILED " %u  " VOTE_ZH_TOTAL " %u",
                   (unsigned)valid,
                   (unsigned)vote_status_spoiled(),
                   (unsigned)(valid + vote_status_spoiled()));
    draw_string(lcd, 8U, (uint16_t)(VOTE_MENU_BODY_Y + 68U), line, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_SM);

    if (phase != NULL && strcmp(phase, "voting") == 0) {
        draw_business_foot(lcd, VOTE_ZH_FOOT_HOME_VOTING);
    } else {
        draw_business_foot(lcd, VOTE_ZH_FOOT_HOME);
    }
    draw_admin_entry_progress(lcd, admin_progress);
}

static void draw_voting_screen(st7789_t *lcd)
{
    char line[48];
    uint16_t valid = 0U;
    uint8_t i;
    uint8_t count = vote_status_candidate_count();
    int countdown = 0;

    (void)vote_status_current_phase(&countdown);

    for (i = 0U; i < count; i++) {
        valid = (uint16_t)(valid + vote_status_votes(i));
    }

    if (countdown > 0) {
        draw_voting_title_bar(lcd, countdown);
    } else {
        draw_business_title(lcd, VOTE_ZH_VOTING);
    }
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 16U), VOTE_ZH_READY, VOTE_MENU_COLOR_TEXT,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    (void)snprintf(line, sizeof(line), VOTE_ZH_VALID ":%u " VOTE_ZH_SPOILED ":%u", (unsigned)valid,
                   (unsigned)vote_status_spoiled());
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 34U), line, VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                       VOTE_MENU_FONT_SM);
    draw_business_foot(lcd, VOTE_ZH_FOOT_VOTING);
}

static void draw_select_screen(st7789_t *lcd, uint8_t focus, uint8_t timeout_sec)
{
    uint8_t count = vote_status_candidate_count();
    uint8_t i;
    uint8_t vis_first;
    uint8_t vis_last;
    uint8_t vis_n;
    const uint16_t card_w = 68U;
    const uint16_t card_h = 44U;
    const uint16_t card_y = (uint16_t)(VOTE_MENU_BODY_Y + 18U);
    const uint16_t gap    = 6U;
    uint16_t total_w;
    uint16_t x0;

    if (count == 0U) {
        count = 1U;
    }
    if (focus >= count) {
        focus = 0U;
    }

    vis_first = (focus > 0U) ? (uint8_t)(focus - 1U) : 0U;
    vis_last  = (focus + 1U < count) ? (uint8_t)(focus + 1U) : (uint8_t)(count - 1U);
    vis_n     = (uint8_t)(vis_last - vis_first + 1U);
    total_w   = (uint16_t)((uint16_t)vis_n * card_w + (uint16_t)(vis_n - 1U) * gap);
    x0        = (VOTE_MENU_LCD_W > total_w) ? (uint16_t)((VOTE_MENU_LCD_W - total_w) / 2U) : 0U;

    draw_select_title_bar(lcd, timeout_sec);
    lcd_fill(lcd, 0U, VOTE_MENU_BODY_Y, VOTE_MENU_LCD_W, VOTE_MENU_FOOT_Y, VOTE_MENU_COLOR_BG);

    for (i = vis_first; i <= vis_last; i++) {
        const uint16_t col = (uint16_t)(i - vis_first);
        const uint16_t x   = (uint16_t)(x0 + col * (card_w + gap));
        const bool is_focus = (i == focus);
        uint16_t bg;
        uint16_t fc;
        uint16_t bc;
        const char *name;

        bg = is_focus ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_SURFACE;
        fc = is_focus ? VOTE_MENU_COLOR_TEXT : VOTE_MENU_COLOR_MUTED;
        bc = is_focus ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_BORDER;

        lcd_fill(lcd, x, card_y, (uint16_t)(x + card_w), (uint16_t)(card_y + card_h), bg);
        lcd_draw_rectangle(lcd, x, card_y, (uint16_t)(x + card_w - 1U), (uint16_t)(card_y + card_h - 1U), bc);
        if (is_focus) {
            lcd_fill(lcd, x, card_y, (uint16_t)(x + card_w), (uint16_t)(card_y + 3U), VOTE_MENU_COLOR_ACCENT);
        }

        name = vote_status_candidate_lcd_name(i);
        draw_string_center_in_box(lcd, x, card_y, card_w, card_h, name, fc, bg, VOTE_MENU_FONT_SM);
    }

    if (focus > 0U) {
        draw_string(lcd, 6U, (uint16_t)(card_y + 14U), "<", VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                    VOTE_MENU_FONT_MD);
    }
    if (focus + 1U < count) {
        draw_string(lcd, 228U, (uint16_t)(card_y + 14U), ">", VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                    VOTE_MENU_FONT_MD);
    }

    if (count > 1U) {
        const uint16_t dot_y   = (uint16_t)(VOTE_MENU_BODY_Y + 72U);
        const uint16_t span    = (uint16_t)((count - 1U) * 10U);
        const uint16_t track_x = (uint16_t)((VOTE_MENU_LCD_W - span) / 2U);

        for (i = 0U; i < count; i++) {
            const uint16_t dx = (uint16_t)(track_x + i * 10U);
            const uint16_t dc = (i == focus) ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_BORDER;
            lcd_fill(lcd, dx, dot_y, (uint16_t)(dx + 5U), (uint16_t)(dot_y + 5U), dc);
        }
    }

    draw_business_foot(lcd, select_foot_hint());
}

static const char *spoiled_type_lcd_label(vote_spoiled_type_e type)
{
    switch (type) {
    case VOTE_SPOILED_BLANK:
        return VOTE_ZH_BLANK;
    case VOTE_SPOILED_MULTIPLE:
        return VOTE_ZH_MULTIPLE;
    case VOTE_SPOILED_IRREGULAR:
        return VOTE_ZH_IRREGULAR;
    default:
        return VOTE_ZH_SPOILED;
    }
}

static void draw_cooldown_screen(st7789_t *lcd, uint8_t sec, vote_spoiled_type_e spoiled)
{
    char buf[8];

    if (spoiled != VOTE_SPOILED_NONE) {
        draw_business_title(lcd, VOTE_ZH_SPOILED);
        draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 16U), spoiled_type_lcd_label(spoiled),
                           VOTE_MENU_COLOR_DANGER, VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    } else {
        draw_business_title(lcd, VOTE_ZH_WAIT);
    }
    (void)snprintf(buf, sizeof(buf), "%02u", (unsigned)sec);
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 36U), buf, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_BG,
                       VOTE_MENU_FONT_MD);
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 56U), VOTE_ZH_COOLDOWN, VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 72U), VOTE_ZH_NO_APPROACH, VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    draw_business_foot(lcd, VOTE_ZH_FOOT_COOLDOWN);
}

static void draw_violation_screen(st7789_t *lcd)
{
    draw_business_title(lcd, VOTE_ZH_VIOLATION);
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 24U), VOTE_ZH_DUP_VOTE, VOTE_MENU_COLOR_DANGER,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 44U), VOTE_ZH_WAIT, VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    draw_business_foot(lcd, VOTE_ZH_FOOT_ALARM);
}

static void draw_locked_screen(st7789_t *lcd, uint8_t scroll_idx, uint8_t admin_progress)
{
    char line[48];
    uint8_t count = vote_status_candidate_count();
    uint8_t i;
    uint16_t valid = 0U;
    uint16_t max_v = 1U;
    uint16_t y;
    const uint8_t visible = 3U;
    uint8_t start;
    const uint16_t row_h    = 16U;
    const uint16_t list_y0  = (uint16_t)(VOTE_MENU_BODY_Y + 18U);
    const uint16_t track_h  = (uint16_t)(visible * row_h - 2U);

    if (count == 0U) {
        count = 1U;
    }
    if (scroll_idx >= count) {
        scroll_idx = 0U;
    }

    start = scroll_idx;
    if (count > visible && start + visible > count) {
        start = (uint8_t)(count - visible);
    }

    for (i = 0U; i < count; i++) {
        uint16_t v = vote_status_votes(i);
        valid      = (uint16_t)(valid + v);
        if (v > max_v) {
            max_v = v;
        }
    }

    draw_business_title(lcd, VOTE_ZH_LOCKED);
    draw_string(lcd, 8U, (uint16_t)(VOTE_MENU_BODY_Y + 4U), VOTE_ZH_FINAL, VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_SM);
    y = list_y0;

    for (i = start; i < count && i < (uint8_t)(start + visible); i++) {
        char vbuf[8];
        uint16_t v   = vote_status_votes(i);
        uint16_t bar = (uint16_t)(v * 80U / max_v);
        uint16_t bg  = (i == scroll_idx) ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_BG;
        uint16_t fc  = VOTE_MENU_COLOR_TEXT;

        lcd_fill(lcd, 0U, y, VOTE_MENU_LCD_W, (uint16_t)(y + row_h), bg);
        if (i == scroll_idx) {
            lcd_fill(lcd, 0U, y, 3U, (uint16_t)(y + row_h), VOTE_MENU_COLOR_ACCENT);
        }
        draw_string(lcd, 8U, y, vote_status_candidate_lcd_name(i), fc, bg, VOTE_MENU_FONT_SM);
        lcd_fill(lcd, 60U, (uint16_t)(y + 4U), (uint16_t)(60U + bar), (uint16_t)(y + 10U), VOTE_MENU_COLOR_ACCENT);
        (void)snprintf(vbuf, sizeof(vbuf), "%u", (unsigned)v);
        draw_string(lcd, 200U, y, vbuf, VOTE_MENU_COLOR_MUTED, bg, VOTE_MENU_FONT_SM);
        y = (uint16_t)(y + row_h);
    }

    draw_simple_scrollbar(lcd, list_y0, track_h, count, visible, scroll_idx);

    (void)snprintf(line, sizeof(line), VOTE_ZH_VALID ":%u " VOTE_ZH_SPOILED ":%u", (unsigned)valid,
                   (unsigned)vote_status_spoiled());
    draw_string(lcd, 8U, (uint16_t)(VOTE_MENU_BODY_Y + 78U), line, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_SM);

    if (count > visible) {
        draw_business_foot(lcd, VOTE_ZH_FOOT_LOCKED_SCROLL);
    } else {
        draw_business_foot(lcd, VOTE_ZH_FOOT_LOCKED_END);
    }
    draw_admin_entry_progress(lcd, admin_progress);
}

static uint8_t list_viewport_start(uint8_t focus_idx, uint8_t total, uint8_t visible)
{
    uint8_t start = 0U;

    if (total <= visible || visible == 0U) {
        return 0U;
    }
    if (focus_idx >= visible) {
        start = (uint8_t)(focus_idx - visible + 1U);
    }
    if (start + visible > total) {
        start = (uint8_t)(total - visible);
    }
    return start;
}

static void draw_history_screen(st7789_t *lcd, uint8_t focus_idx, uint8_t admin_progress)
{
    char time_buf[16];
    char detail[48];
    char title[32];
    const vote_history_entry_t *rec = NULL;
    const uint8_t visible = (uint8_t)VOTE_MENU_VISIBLE_ROWS;
    const uint16_t row_h  = 20U;
    const uint16_t list_y0 = (uint16_t)(VOTE_MENU_BODY_Y + 4U);
    const uint16_t track_h = (uint16_t)(visible * row_h - 2U);
    uint8_t total = vote_history_count();
    uint8_t i;
    uint8_t start;
    uint16_t y;

    draw_business_title(lcd, VOTE_ZH_HISTORY);
    (void)snprintf(title, sizeof(title), "%s %u", VOTE_ZH_HISTORY, (unsigned)total);
    draw_string(lcd, 72U, 4U, title, VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);

    lcd_fill(lcd, 0U, VOTE_MENU_BODY_Y, VOTE_MENU_LCD_W, VOTE_MENU_FOOT_Y, VOTE_MENU_COLOR_BG);

    if (total == 0U) {
        draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 28U), VOTE_ZH_NO_HISTORY, VOTE_MENU_COLOR_MUTED,
                           VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
        draw_business_foot(lcd, VOTE_ZH_FOOT_HISTORY);
        return;
    }

    if (focus_idx >= total) {
        focus_idx = 0U;
    }

    start = list_viewport_start(focus_idx, total, visible);

    y = list_y0;
    for (i = start; i < total && i < (uint8_t)(start + visible); i++) {
        uint16_t bg;
        uint16_t fc;

        if (!vote_history_get_display(i, &rec) || rec == NULL) {
            break;
        }

        bg = (i == focus_idx) ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_BG;
        fc = (i == focus_idx) ? VOTE_MENU_COLOR_TEXT : VOTE_MENU_COLOR_MUTED;

        vote_history_format_time(rec, time_buf, sizeof(time_buf));
        vote_history_format_detail(rec, detail, sizeof(detail));

        lcd_fill(lcd, 0U, y, VOTE_MENU_LCD_W, (uint16_t)(y + row_h - 1U), bg);
        if (i == focus_idx) {
            lcd_fill(lcd, 0U, y, 3U, (uint16_t)(y + row_h - 1U), VOTE_MENU_COLOR_ACCENT);
        }
        draw_string(lcd, 8U, (uint16_t)(y + 4U), time_buf, fc, bg, VOTE_MENU_FONT_SM);
        draw_string(lcd, 72U, (uint16_t)(y + 4U), detail, fc, bg, VOTE_MENU_FONT_SM);
        y = (uint16_t)(y + row_h);
    }

    draw_simple_scrollbar(lcd, list_y0, track_h, total, visible, focus_idx);

    if (total > visible) {
        draw_business_foot(lcd, VOTE_ZH_FOOT_HISTORY_SCROLL);
    } else {
        draw_business_foot(lcd, VOTE_ZH_FOOT_HISTORY);
    }
    draw_admin_entry_progress(lcd, admin_progress);
}

void vote_lcd_draw(st7789_t *lcd, const vote_lcd_ctx_t *ctx)
{
    if (lcd == NULL || ctx == NULL) {
        return;
    }

    if (vote_lcd_screen_is_menu(ctx->screen)) {
        if (ctx->menu_eng != NULL) {
            vote_menu_lcd_draw(lcd, ctx->menu_eng);
        }
        return;
    }

    lcd_fill(lcd, 0U, 0U, VOTE_MENU_LCD_W, VOTE_MENU_LCD_H, VOTE_MENU_COLOR_BG);
    lcd_fill(lcd, 0U, VOTE_MENU_BODY_Y, VOTE_MENU_LCD_W, VOTE_MENU_FOOT_Y, VOTE_MENU_COLOR_BG);

    switch (ctx->screen) {
    case VOTE_LCD_SCREEN_HOME:
        draw_home_screen(lcd, ctx->home_scroll_idx, ctx->admin_entry_progress);
        break;
    case VOTE_LCD_SCREEN_VOTING:
        draw_voting_screen(lcd);
        break;
    case VOTE_LCD_SCREEN_SELECT:
        draw_select_screen(lcd, ctx->select_idx, ctx->select_timeout_sec);
        break;
    case VOTE_LCD_SCREEN_COOLDOWN:
        draw_cooldown_screen(lcd, ctx->cooldown_sec, ctx->spoiled_type);
        break;
    case VOTE_LCD_SCREEN_VIOLATION:
        draw_violation_screen(lcd);
        break;
    case VOTE_LCD_SCREEN_LOCKED:
        draw_locked_screen(lcd, ctx->locked_scroll_idx, ctx->admin_entry_progress);
        break;
    case VOTE_LCD_SCREEN_HISTORY:
        draw_history_screen(lcd, ctx->history_idx, ctx->admin_entry_progress);
        break;
    default:
        draw_home_screen(lcd, ctx->home_scroll_idx, ctx->admin_entry_progress);
        break;
    }
}

void vote_lcd_draw_select_title(st7789_t *lcd, uint8_t timeout_sec)
{
    if (lcd == NULL) {
        return;
    }
    draw_select_title_bar(lcd, timeout_sec);
}

void vote_lcd_draw_clock(st7789_t *lcd, const vote_lcd_ctx_t *ctx)
{
    if (lcd == NULL || ctx == NULL) {
        return;
    }

    if (vote_lcd_screen_is_menu(ctx->screen)) {
        if (ctx->menu_eng != NULL) {
            vote_menu_lcd_draw_clock(lcd);
        }
        return;
    }

    if (ctx->screen == VOTE_LCD_SCREEN_SELECT) {
        vote_lcd_draw_select_title(lcd, ctx->select_timeout_sec);
        return;
    }

    if (ctx->screen == VOTE_LCD_SCREEN_HOME) {
        int cd = 0;
        const char *phase = vote_status_current_phase(&cd);

        if (phase != NULL && strcmp(phase, "waiting") == 0 && cd > 0) {
            draw_waiting_title_bar(lcd, cd);
            return;
        }
    }

    if (ctx->screen == VOTE_LCD_SCREEN_VOTING) {
        int cd = 0;

        (void)vote_status_current_phase(&cd);
        if (cd > 0) {
            draw_voting_title_bar(lcd, cd);
            return;
        }
    }

    {
        char clk[16];

        format_clock_text(clk, sizeof(clk));
        lcd_fill(lcd, VOTE_MENU_TITLE_CLK_X, 4U, title_clock_end_x(clk), 16U, VOTE_MENU_COLOR_SURFACE);
        draw_string(lcd, VOTE_MENU_TITLE_CLK_X, 4U, clk, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE,
                    VOTE_MENU_FONT_SM);
    }
}

void vote_menu_lcd_draw(st7789_t *lcd, const menu_engine_t *eng)
{
    const menu_page_t *page;
    vote_menu_page_id_t pid;

    if (lcd == NULL || eng == NULL) {
        return;
    }

    page = menu_current_page(eng);
    pid  = page_id_of(page);

    lcd_fill(lcd, 0U, 0U, VOTE_MENU_LCD_W, VOTE_MENU_LCD_H, VOTE_MENU_COLOR_BG);
    draw_title_bar(lcd, page);
    lcd_fill(lcd, 0U, VOTE_MENU_BODY_Y, VOTE_MENU_LCD_W, VOTE_MENU_FOOT_Y, VOTE_MENU_COLOR_BG);

    switch (pid) {
    case VOTE_MENU_PAGE_TIME:
        draw_time_page(lcd, eng);
        break;
    case VOTE_MENU_PAGE_COUNT:
        draw_count_page(lcd, eng);
        break;
    case VOTE_MENU_PAGE_COOLDOWN:
        draw_cooldown_page(lcd, eng);
        break;
    case VOTE_MENU_PAGE_RESET:
        draw_reset_modal(lcd, eng, page);
        break;
    default:
        draw_list_page(lcd, eng, page);
        break;
    }

    draw_foot(lcd, page);
}

void vote_menu_lcd_draw_clock(st7789_t *lcd)
{
    char clk[16];

    if (lcd == NULL || !st7789_is_initialized(lcd)) {
        return;
    }

    format_clock_text(clk, sizeof(clk));
    lcd_fill(lcd, VOTE_MENU_TITLE_CLK_X, 4U, title_clock_end_x(clk), 16U, VOTE_MENU_COLOR_SURFACE);
    draw_string(lcd, VOTE_MENU_TITLE_CLK_X, 4U, clk, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE,
                VOTE_MENU_FONT_SM);
    draw_title_ip(lcd);
}

void vote_menu_lcd_draw_toast(st7789_t *lcd, const char *msg, int is_error)
{
    uint16_t bg;
    uint16_t fc;
    uint16_t y;

    if (lcd == NULL || msg == NULL || msg[0] == '\0') {
        return;
    }

    bg = is_error ? VOTE_MENU_COLOR_DANGER_BG : VOTE_MENU_RGB565(0x1A, 0x3D, 0x2E);
    fc = is_error ? VOTE_MENU_COLOR_DANGER : VOTE_MENU_COLOR_OK;
    y  = (uint16_t)(VOTE_MENU_BODY_Y + VOTE_MENU_BODY_H / 2U - 8U);

    lcd_fill(lcd, 20U, y, 220U, (uint16_t)(y + 20U), bg);
    draw_string_center_in_box(lcd, 20U, y, 200U, 20U, msg, fc, bg, VOTE_MENU_FONT_SM);
}
