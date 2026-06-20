/**
 * @file vote_menu_lcd.c
 * @brief ballot_guard 菜单 LCD 视图（240x135 横屏，对齐菜单 UI 设计稿）。
 */

#include "vote_menu_lcd.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "lcd.h"
#include "vote_menu_config.h"
#include "vote_menu_pages.h"

#define VOTE_MENU_FONT_SM (12U)
#define VOTE_MENU_FONT_MD (16U)

static vote_menu_page_id_t page_id_of(const menu_page_t *page)
{
    if (page == NULL || page->user_ctx == NULL) {
        return VOTE_MENU_PAGE_ADMIN;
    }
    return *(const vote_menu_page_id_t *)page->user_ctx;
}

static uint16_t text_width(const char *s, uint8_t sizey)
{
    size_t n;

    if (s == NULL) {
        return 0U;
    }
    n = strlen(s);
    return (uint16_t)(n * (sizey / 2U));
}

static void draw_string(st7789_t *lcd, uint16_t x, uint16_t y, const char *s, uint16_t fc, uint16_t bc, uint8_t sizey)
{
    if (lcd == NULL || s == NULL) {
        return;
    }
    lcd_show_string(lcd, x, y, (const uint8_t *)s, fc, bc, sizey, 0U);
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
    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t h   = (sec / 3600U) % 24U;
    uint32_t m   = (sec / 60U) % 60U;
    uint32_t s2  = sec % 60U;

    if (clk == NULL || clk_len == 0U) {
        return;
    }
    (void)snprintf(clk, clk_len, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s2);
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
    draw_string(lcd, 4U, 4U, clk, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);

    title = (page != NULL && page->title != NULL) ? page->title : "Menu";
    title_x = (uint16_t)((VOTE_MENU_LCD_W - text_width(title, VOTE_MENU_FONT_SM)) / 2U);
    draw_string(lcd, title_x, 4U, title, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
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
                          size_t aux_len)
{
    uint16_t bg  = VOTE_MENU_COLOR_BG;
    uint16_t fc  = VOTE_MENU_COLOR_TEXT;
    uint16_t lnc = VOTE_MENU_COLOR_BG;
    const char *label;
    char prefix[4];

    if (item == NULL) {
        return;
    }

    if (focus != 0U) {
        bg  = menu_item_is_danger(item) ? VOTE_MENU_COLOR_DANGER_BG : VOTE_MENU_COLOR_SURFACE2;
        lnc = menu_item_is_danger(item) ? VOTE_MENU_COLOR_DANGER : VOTE_MENU_COLOR_ACCENT;
        fc  = menu_item_is_danger(item) ? VOTE_MENU_COLOR_DANGER : VOTE_MENU_COLOR_TEXT;
    } else if (menu_item_is_danger(item)) {
        fc = VOTE_MENU_COLOR_DANGER;
    }

    lcd_fill(lcd, 0U, y, VOTE_MENU_LCD_W, (uint16_t)(y + VOTE_MENU_ROW_H), bg);
    if (lnc != VOTE_MENU_COLOR_BG) {
        lcd_fill(lcd, 0U, y, 3U, (uint16_t)(y + VOTE_MENU_ROW_H), lnc);
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

static void draw_list_page(st7789_t *lcd, const menu_engine_t *eng, const menu_page_t *page)
{
    char aux_buf[32];
    uint16_t first_slot = 0U;
    uint16_t i;

    (void)menu_page_viewport_first_focus(page, menu_current_index(eng), VOTE_MENU_VISIBLE_ROWS, &first_slot);

    for (i = 0U; i < VOTE_MENU_VISIBLE_ROWS; i++) {
        uint16_t item_index = 0U;
        uint16_t y          = (uint16_t)(VOTE_MENU_BODY_Y + i * VOTE_MENU_ROW_H);
        const menu_item_t *item;

        if (!menu_page_focus_item_at(page, (uint16_t)(first_slot + i), &item_index)) {
            lcd_fill(lcd, 0U, y, VOTE_MENU_LCD_W, (uint16_t)(y + VOTE_MENU_ROW_H), VOTE_MENU_COLOR_BG);
            continue;
        }
        item = &page->items[item_index];
        draw_list_row(lcd, y, item, (uint16_t)(item_index == menu_current_index(eng)), eng->app_ctx, aux_buf,
                      sizeof(aux_buf));
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

    draw_string(lcd, 16U, (uint16_t)(VOTE_MENU_BODY_Y + 4U), "Start", VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_SM);
    (void)snprintf(buf, sizeof(buf), "%02u", (unsigned)st->start_h);
    draw_digit_box(lcd, 24U, (uint16_t)(VOTE_MENU_BODY_Y + 18U), buf, (uint16_t)(idx == 0U));
    draw_string(lcd, 64U, (uint16_t)(VOTE_MENU_BODY_Y + 24U), ":", VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_MD);
    (void)snprintf(buf, sizeof(buf), "%02u", (unsigned)st->start_m);
    draw_digit_box(lcd, 76U, (uint16_t)(VOTE_MENU_BODY_Y + 18U), buf, (uint16_t)(idx == 1U));

    draw_string(lcd, 16U, (uint16_t)(VOTE_MENU_BODY_Y + 50U), "End", VOTE_MENU_COLOR_MUTED, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_SM);
    (void)snprintf(buf, sizeof(buf), "%02u", (unsigned)st->end_h);
    draw_digit_box(lcd, 24U, (uint16_t)(VOTE_MENU_BODY_Y + 64U), buf, (uint16_t)(idx == 2U));
    draw_string(lcd, 64U, (uint16_t)(VOTE_MENU_BODY_Y + 70U), ":", VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_BG,
                VOTE_MENU_FONT_MD);
    (void)snprintf(buf, sizeof(buf), "%02u", (unsigned)st->end_m);
    draw_digit_box(lcd, 76U, (uint16_t)(VOTE_MENU_BODY_Y + 64U), buf, (uint16_t)(idx == 3U));

    {
        uint16_t bx = 80U;
        uint16_t by = (uint16_t)(VOTE_MENU_BODY_Y + 72U);
        uint16_t bg = (idx == 4U) ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_SURFACE;
        lcd_fill(lcd, bx, by, (uint16_t)(bx + 80U), (uint16_t)(by + 24U), bg);
        lcd_draw_rectangle(lcd, bx, by, (uint16_t)(bx + 79U), (uint16_t)(by + 23U),
                           (idx == 4U) ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_BORDER);
        draw_string_center_in_box(lcd, bx, by, 80U, 24U, "Save", VOTE_MENU_COLOR_TEXT, bg, VOTE_MENU_FONT_SM);
    }
}

static void draw_count_page(st7789_t *lcd, const menu_engine_t *eng)
{
    vote_menu_settings_t *st = vote_menu_settings();
    char buf[8];
    uint16_t idx             = menu_current_index(eng);

    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 8U), "Candidates", VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    (void)snprintf(buf, sizeof(buf), "%u", (unsigned)st->candidate_count);
    {
        uint16_t bx = 88U;
        uint16_t by = (uint16_t)(VOTE_MENU_BODY_Y + 28U);
        uint16_t bg = (idx == 0U) ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_SURFACE;
        lcd_fill(lcd, bx, by, (uint16_t)(bx + 64U), (uint16_t)(by + 40U), bg);
        lcd_draw_rectangle(lcd, bx, by, (uint16_t)(bx + 63U), (uint16_t)(by + 39U),
                           (idx == 0U) ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_BORDER);
        draw_string_center_in_box(lcd, bx, by, 64U, 40U, buf, VOTE_MENU_COLOR_TEXT, bg, VOTE_MENU_FONT_MD);
    }
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 72U), "Range 2-6", VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    {
        uint16_t bx = 80U;
        uint16_t by = (uint16_t)(VOTE_MENU_BODY_Y + 52U);
        uint16_t bg = (idx == 1U) ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_SURFACE;
        lcd_fill(lcd, bx, by, (uint16_t)(bx + 80U), (uint16_t)(by + 24U), bg);
        lcd_draw_rectangle(lcd, bx, by, (uint16_t)(bx + 79U), (uint16_t)(by + 23U),
                           (idx == 1U) ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_BORDER);
        draw_string_center_in_box(lcd, bx, by, 80U, 24U, "Save", VOTE_MENU_COLOR_TEXT, bg, VOTE_MENU_FONT_SM);
    }
}

static void draw_cooldown_page(st7789_t *lcd, const menu_engine_t *eng)
{
    vote_menu_settings_t *st = vote_menu_settings();
    char buf[8];
    uint16_t idx             = menu_current_index(eng);
    uint8_t v;

    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 4U), "Cooldown (sec)", VOTE_MENU_COLOR_MUTED,
                       VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
    for (v = 3U; v <= 10U; v++) {
        uint16_t x = (uint16_t)(20U + (v - 3U) * 26U);
        uint16_t fc =
            (idx == 0U && v == st->cooldown_sec) ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_MUTED;
        char digit[4];
        (void)snprintf(digit, sizeof(digit), "%u", (unsigned)v);
        draw_string(lcd, x, (uint16_t)(VOTE_MENU_BODY_Y + 22U), digit, fc, VOTE_MENU_COLOR_BG, VOTE_MENU_FONT_SM);
        if (idx == 0U && v == st->cooldown_sec) {
            lcd_fill(lcd, (uint16_t)(x + 2U), (uint16_t)(VOTE_MENU_BODY_Y + 34U), (uint16_t)(x + 14U),
                     (uint16_t)(VOTE_MENU_BODY_Y + 38U), VOTE_MENU_COLOR_ACCENT);
        }
    }
    (void)snprintf(buf, sizeof(buf), "%u", (unsigned)st->cooldown_sec);
    draw_string_center(lcd, (uint16_t)(VOTE_MENU_BODY_Y + 44U), buf, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_BG,
                       VOTE_MENU_FONT_MD);
    {
        uint16_t bx = 80U;
        uint16_t by = (uint16_t)(VOTE_MENU_BODY_Y + 64U);
        uint16_t bg = (idx == 1U) ? VOTE_MENU_COLOR_SURFACE2 : VOTE_MENU_COLOR_SURFACE;
        lcd_fill(lcd, bx, by, (uint16_t)(bx + 80U), (uint16_t)(by + 24U), bg);
        lcd_draw_rectangle(lcd, bx, by, (uint16_t)(bx + 79U), (uint16_t)(by + 23U),
                           (idx == 1U) ? VOTE_MENU_COLOR_ACCENT : VOTE_MENU_COLOR_BORDER);
        draw_string_center_in_box(lcd, bx, by, 80U, 24U, "Save", VOTE_MENU_COLOR_TEXT, bg, VOTE_MENU_FONT_SM);
    }
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

    draw_string(lcd, 18U, (uint16_t)(VOTE_MENU_BODY_Y + 18U), "Reset all vote data?", VOTE_MENU_COLOR_TEXT,
                VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
    draw_string(lcd, 18U, (uint16_t)(VOTE_MENU_BODY_Y + 34U), "Cannot undo.", VOTE_MENU_COLOR_MUTED,
                VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);

    if (page != NULL && page->items != NULL && page->count >= 2U) {
        uint16_t y = (uint16_t)(VOTE_MENU_BODY_Y + 54U);
        draw_list_row(lcd, y, &page->items[0], (uint16_t)(idx == 0U), eng->app_ctx, aux_buf, sizeof(aux_buf));
        draw_list_row(lcd, (uint16_t)(y + VOTE_MENU_ROW_H), &page->items[1], (uint16_t)(idx == 1U), eng->app_ctx,
                      aux_buf, sizeof(aux_buf));
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
    /* 设计稿顶栏时钟区约 72px 宽；局部填充避免整屏刷新。 */
    lcd_fill(lcd, 4U, 4U, 76U, 16U, VOTE_MENU_COLOR_SURFACE);
    draw_string(lcd, 4U, 4U, clk, VOTE_MENU_COLOR_TEXT, VOTE_MENU_COLOR_SURFACE, VOTE_MENU_FONT_SM);
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
