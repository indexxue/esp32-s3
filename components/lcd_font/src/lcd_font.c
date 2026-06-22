/**
 * @file lcd_font.c
 * @brief ballot_guard 扩展 GB2312 字库 + 混合文本绘制。
 */

#include "lcd_font.h"

#include "ballot_gb12.h"
#include "lcd.h"

static const ballot_gb12_glyph_t *find_gb12(const uint8_t *s)
{
    uint16_t i;

    if (s == NULL || s[0] == 0U || s[1] == 0U) {
        return NULL;
    }
    for (i = 0U; i < BALLOT_GB12_COUNT; i++) {
        if (ballot_gb12[i].Index[0] == s[0] && ballot_gb12[i].Index[1] == s[1]) {
            return &ballot_gb12[i];
        }
    }
    return NULL;
}

static bool is_gb2312_pair(const uint8_t *p)
{
    if (p == NULL || p[0] == 0U || p[1] == 0U) {
        return false;
    }
    return (p[0] >= 0xA1U && p[0] <= 0xF7U && p[1] >= 0xA1U && p[1] <= 0xFEU);
}

static void draw_gb12_glyph(st7789_t *lcd,
                            uint16_t x,
                            uint16_t y,
                            const ballot_gb12_glyph_t *g,
                            uint16_t fc,
                            uint16_t bc,
                            uint8_t sizey,
                            uint8_t mode)
{
    uint16_t i;
    uint8_t j;
    uint8_t m = 0U;
    uint16_t x0 = x;

    if (lcd == NULL || g == NULL || sizey != 12U) {
        return;
    }

    if (st7789_set_window(lcd, x, y, (uint16_t)(x + sizey - 1U), (uint16_t)(y + sizey - 1U)) != ST7789_OK) {
        st7789_end_write(lcd);
        return;
    }

    for (i = 0U; i < 24U; i++) {
        for (j = 0U; j < 8U; j++) {
            if (mode == 0U) {
                if ((g->Msk[i] & (uint8_t)(1U << j)) != 0U) {
                    lcd_wr_rgb565(lcd, fc);
                } else {
                    lcd_wr_rgb565(lcd, bc);
                }
                m++;
                if ((m % sizey) == 0U) {
                    m = 0U;
                    break;
                }
            } else {
                if ((g->Msk[i] & (uint8_t)(1U << j)) != 0U) {
                    lcd_draw_point(lcd, x, y, fc);
                }
                x++;
                if ((x - x0) == sizey) {
                    x = x0;
                    y++;
                    break;
                }
            }
        }
    }
    st7789_end_write(lcd);
}

static bool is_gb2312_lead(uint8_t b)
{
    return (b >= 0xA1U && b <= 0xF7U);
}

uint16_t lcd_font_text_width(const char *text, uint8_t sizey)
{
    const uint8_t *p = (const uint8_t *)text;
    uint16_t w       = 0U;
    const uint16_t ascii_step = (uint16_t)(sizey / 2U);

    if (p == NULL) {
        return 0U;
    }

    while (*p != 0U) {
        if (is_gb2312_pair(p)) {
            w = (uint16_t)(w + sizey);
            p += 2U;
        } else {
            w = (uint16_t)(w + ascii_step);
            p++;
        }
    }
    return w;
}

void lcd_font_draw_text(st7789_t *lcd,
                        uint16_t x,
                        uint16_t y,
                        const char *text,
                        uint16_t fc,
                        uint16_t bc,
                        uint8_t sizey,
                        uint8_t mode)
{
    const uint8_t *p = (const uint8_t *)text;
    const ballot_gb12_glyph_t *g;
    const uint16_t ascii_step = (uint16_t)(sizey / 2U);

    if (lcd == NULL || p == NULL) {
        return;
    }

    while (*p != 0U) {
        if (is_gb2312_pair(p)) {
            if (sizey == 12U) {
                g = find_gb12(p);
                if (g != NULL) {
                    draw_gb12_glyph(lcd, x, y, g, fc, bc, sizey, mode);
                }
            } else {
                lcd_show_chinese(lcd, x, y, (uint8_t *)p, fc, bc, sizey, mode);
            }
            x = (uint16_t)(x + sizey);
            p += 2U;
        } else {
            lcd_show_char(lcd, x, y, *p, fc, bc, sizey, mode);
            x = (uint16_t)(x + ascii_step);
            p++;
        }
    }
}
