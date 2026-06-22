/**
 * @file    lcd.c
 * @brief   由 `tmp/LCD/lcd.c` 移植的绘图与字库，底层通过 `st7789_*` 发送像素。
 */

#include "lcd.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lcdfont.h"

#define LCD_LINEBUF_MAX 320U
/** 与 SPI `max_transfer_sz` 对齐的全屏纯色块填充（减少事务数，便于 DMA 吞吐）。 */
#define LCD_FAST_FILL_BLK 32768U

static uint16_t s_linebuf[LCD_LINEBUF_MAX];
static uint8_t s_fast_fill_blk[LCD_FAST_FILL_BLK];

void lcd_wr_rgb565(st7789_t *lcd, uint16_t color) {
    (void)st7789_write_pixels(lcd, &color, 1U);
}

void lcd_fill(st7789_t *lcd, uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color) {
    uint16_t row_w;
    uint16_t row;

    if (lcd == NULL || !st7789_is_initialized(lcd) || xend <= xsta || yend <= ysta) {
        return;
    }
    row_w = (uint16_t)(xend - xsta);
    if (row_w > LCD_LINEBUF_MAX) {
        return;
    }
    for (uint16_t i = 0; i < row_w; i++) {
        s_linebuf[i] = color;
    }
    if (st7789_set_window(lcd, xsta, ysta, (uint16_t)(xend - 1U), (uint16_t)(yend - 1U)) != ST7789_OK) {
        return;
    }
    for (row = ysta; row < yend; row++) {
        if (st7789_write_pixels(lcd, s_linebuf, row_w) != ST7789_OK) {
            break;
        }
    }
    st7789_end_write(lcd);
}

/**
 * 纯色矩形：在 `st7789_set_window` 后按行打包至多 32KiB/次写显存，显著少于 `lcd_fill` 的逐行 SPI 次数。
 * 需与板级 SPI `max_transfer_sz`（如 `BOARD_ST7789_SPI_MAX_TX`）一致或更小。
 */
void lcd_fill_fast(st7789_t *lcd, uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color) {
    uint16_t row_w;
    uint16_t row_bytes;
    uint8_t linebe[LCD_LINEBUF_MAX * 2U];
    uint16_t i;

    if (lcd == NULL || !st7789_is_initialized(lcd) || xend <= xsta || yend <= ysta) {
        return;
    }
    row_w = (uint16_t)(xend - xsta);
    if (row_w > LCD_LINEBUF_MAX) {
        return;
    }
    row_bytes = (uint16_t)(row_w * 2U);
    if (row_bytes == 0U) {
        return;
    }

    for (i = 0; i < row_w; i++) {
        linebe[(size_t)i * 2U] = (uint8_t)(color >> 8);
        linebe[(size_t)i * 2U + 1U] = (uint8_t)(color & 0xFFU);
    }

    if (st7789_set_window(lcd, xsta, ysta, (uint16_t)(xend - 1U), (uint16_t)(yend - 1U)) != ST7789_OK) {
        return;
    }

    {
        uint16_t y = ysta;
        const uint32_t row_b = (uint32_t)row_bytes;
        const uint32_t rows_fit = LCD_FAST_FILL_BLK / row_b;

        if (rows_fit == 0U) {
            st7789_end_write(lcd);
            return;
        }

        while (y < yend) {
            uint32_t rows_batch = (uint32_t)(yend - y);
            if (rows_batch > rows_fit) {
                rows_batch = rows_fit;
            }
            {
                uint32_t nbytes = rows_batch * row_b;
                uint8_t *p = s_fast_fill_blk;
                uint32_t r;

                for (r = 0; r < rows_batch; r++) {
                    (void)memcpy(p, linebe, (size_t)row_bytes);
                    p += row_bytes;
                }
                if (st7789_write_pixel_bytes(lcd, s_fast_fill_blk, nbytes) != ST7789_OK) {
                    break;
                }
            }
            y = (uint16_t)(y + (uint16_t)rows_batch);
        }
    }
    st7789_end_write(lcd);
}

void lcd_draw_point(st7789_t *lcd, uint16_t x, uint16_t y, uint16_t color) {
    if (lcd == NULL || !st7789_is_initialized(lcd)) {
        return;
    }
    if (st7789_set_window(lcd, x, y, x, y) != ST7789_OK) {
        return;
    }
    lcd_wr_rgb565(lcd, color);
    st7789_end_write(lcd);
}

void lcd_draw_line(st7789_t *lcd, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color) {
    uint16_t t;
    int xerr = 0;
    int yerr = 0;
    int delta_x;
    int delta_y;
    int distance;
    int incx;
    int incy;
    int u_row;
    int u_col;

    if (lcd == NULL || !st7789_is_initialized(lcd)) {
        return;
    }

    delta_x = (int)x2 - (int)x1;
    delta_y = (int)y2 - (int)y1;
    u_row = (int)x1;
    u_col = (int)y1;
    if (delta_x > 0) {
        incx = 1;
    } else if (delta_x == 0) {
        incx = 0;
    } else {
        incx = -1;
        delta_x = -delta_x;
    }
    if (delta_y > 0) {
        incy = 1;
    } else if (delta_y == 0) {
        incy = 0;
    } else {
        incy = -1;
        delta_y = -delta_y;
    }
    if (delta_x > delta_y) {
        distance = delta_x;
    } else {
        distance = delta_y;
    }
    for (t = 0; t <= (uint16_t)distance; t++) {
        lcd_draw_point(lcd, (uint16_t)u_row, (uint16_t)u_col, color);
        xerr += delta_x;
        yerr += delta_y;
        if (xerr > distance) {
            xerr -= distance;
            u_row += incx;
        }
        if (yerr > distance) {
            yerr -= distance;
            u_col += incy;
        }
    }
}

void lcd_draw_rectangle(st7789_t *lcd, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color) {
    lcd_draw_line(lcd, x1, y1, x2, y1, color);
    lcd_draw_line(lcd, x1, y1, x1, y2, color);
    lcd_draw_line(lcd, x1, y2, x2, y2, color);
    lcd_draw_line(lcd, x2, y1, x2, y2, color);
}

void lcd_draw_circle(st7789_t *lcd, uint16_t x0, uint16_t y0, uint8_t r, uint16_t color) {
    int a = 0;
    int b = (int)r;

    if (lcd == NULL || !st7789_is_initialized(lcd)) {
        return;
    }

    while (a <= b) {
        lcd_draw_point(lcd, (uint16_t)(x0 - (uint16_t)b), (uint16_t)(y0 - (uint16_t)a), color);
        lcd_draw_point(lcd, (uint16_t)(x0 + (uint16_t)b), (uint16_t)(y0 - (uint16_t)a), color);
        lcd_draw_point(lcd, (uint16_t)(x0 - (uint16_t)a), (uint16_t)(y0 + (uint16_t)b), color);
        lcd_draw_point(lcd, (uint16_t)(x0 - (uint16_t)a), (uint16_t)(y0 - (uint16_t)b), color);
        lcd_draw_point(lcd, (uint16_t)(x0 + (uint16_t)b), (uint16_t)(y0 + (uint16_t)a), color);
        lcd_draw_point(lcd, (uint16_t)(x0 + (uint16_t)a), (uint16_t)(y0 - (uint16_t)b), color);
        lcd_draw_point(lcd, (uint16_t)(x0 + (uint16_t)a), (uint16_t)(y0 + (uint16_t)b), color);
        lcd_draw_point(lcd, (uint16_t)(x0 - (uint16_t)b), (uint16_t)(y0 + (uint16_t)a), color);
        a++;
        if (((a * a) + (b * b)) > ((int)r * (int)r)) {
            b--;
        }
    }
}

void lcd_show_chinese_12x12(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey,
                            uint8_t mode) {
    uint16_t i;
    uint8_t j;
    uint8_t m = 0;
    uint16_t k;
    uint16_t hz_num;
    uint16_t face_num;
    uint16_t x0 = x;

    if (lcd == NULL || !st7789_is_initialized(lcd) || s == NULL) {
        return;
    }

    face_num = (uint16_t)((sizey / 8U + ((sizey % 8U) ? 1U : 0U)) * sizey);
    hz_num = (uint16_t)(sizeof(tfont12) / sizeof(tfont12[0]));
    for (k = 0; k < hz_num; k++) {
        if ((tfont12[k].Index[0] == *s) && (tfont12[k].Index[1] == *(s + 1))) {
            if (st7789_set_window(lcd, x, y, (uint16_t)(x + sizey - 1U), (uint16_t)(y + sizey - 1U)) != ST7789_OK) {
                st7789_end_write(lcd);
                return;
            }
            for (i = 0; i < face_num; i++) {
                for (j = 0; j < 8U; j++) {
                    if (mode == 0U) {
                        if ((tfont12[k].Msk[i] & (uint8_t)(1U << j)) != 0U) {
                            lcd_wr_rgb565(lcd, fc);
                        } else {
                            lcd_wr_rgb565(lcd, bc);
                        }
                        m++;
                        if ((m % sizey) == 0U) {
                            m = 0;
                            break;
                        }
                    } else {
                        if ((tfont12[k].Msk[i] & (uint8_t)(1U << j)) != 0U) {
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
    }
}

void lcd_show_chinese_16x16(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey,
                            uint8_t mode) {
    uint16_t i;
    uint8_t j;
    uint8_t m = 0;
    uint16_t k;
    uint16_t hz_num;
    uint16_t face_num;
    uint16_t x0 = x;

    if (lcd == NULL || !st7789_is_initialized(lcd) || s == NULL) {
        return;
    }

    face_num = (uint16_t)((sizey / 8U + ((sizey % 8U) ? 1U : 0U)) * sizey);
    hz_num = (uint16_t)(sizeof(tfont16) / sizeof(tfont16[0]));
    for (k = 0; k < hz_num; k++) {
        if ((tfont16[k].Index[0] == *s) && (tfont16[k].Index[1] == *(s + 1))) {
            if (st7789_set_window(lcd, x, y, (uint16_t)(x + sizey - 1U), (uint16_t)(y + sizey - 1U)) != ST7789_OK) {
                st7789_end_write(lcd);
                return;
            }
            for (i = 0; i < face_num; i++) {
                for (j = 0; j < 8U; j++) {
                    if (mode == 0U) {
                        if ((tfont16[k].Msk[i] & (uint8_t)(1U << j)) != 0U) {
                            lcd_wr_rgb565(lcd, fc);
                        } else {
                            lcd_wr_rgb565(lcd, bc);
                        }
                        m++;
                        if ((m % sizey) == 0U) {
                            m = 0;
                            break;
                        }
                    } else {
                        if ((tfont16[k].Msk[i] & (uint8_t)(1U << j)) != 0U) {
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
    }
}

void lcd_show_chinese_24x24(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey,
                            uint8_t mode) {
    uint16_t i;
    uint8_t j;
    uint8_t m = 0;
    uint16_t k;
    uint16_t hz_num;
    uint16_t face_num;
    uint16_t x0 = x;

    if (lcd == NULL || !st7789_is_initialized(lcd) || s == NULL) {
        return;
    }

    face_num = (uint16_t)((sizey / 8U + ((sizey % 8U) ? 1U : 0U)) * sizey);
    hz_num = (uint16_t)(sizeof(tfont24) / sizeof(tfont24[0]));
    for (k = 0; k < hz_num; k++) {
        if ((tfont24[k].Index[0] == *s) && (tfont24[k].Index[1] == *(s + 1))) {
            if (st7789_set_window(lcd, x, y, (uint16_t)(x + sizey - 1U), (uint16_t)(y + sizey - 1U)) != ST7789_OK) {
                st7789_end_write(lcd);
                return;
            }
            for (i = 0; i < face_num; i++) {
                for (j = 0; j < 8U; j++) {
                    if (mode == 0U) {
                        if ((tfont24[k].Msk[i] & (uint8_t)(1U << j)) != 0U) {
                            lcd_wr_rgb565(lcd, fc);
                        } else {
                            lcd_wr_rgb565(lcd, bc);
                        }
                        m++;
                        if ((m % sizey) == 0U) {
                            m = 0;
                            break;
                        }
                    } else {
                        if ((tfont24[k].Msk[i] & (uint8_t)(1U << j)) != 0U) {
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
    }
}

void lcd_show_chinese_32x32(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey,
                            uint8_t mode) {
    uint16_t i;
    uint8_t j;
    uint8_t m = 0;
    uint16_t k;
    uint16_t hz_num;
    uint16_t face_num;
    uint16_t x0 = x;

    if (lcd == NULL || !st7789_is_initialized(lcd) || s == NULL) {
        return;
    }

    face_num = (uint16_t)((sizey / 8U + ((sizey % 8U) ? 1U : 0U)) * sizey);
    hz_num = (uint16_t)(sizeof(tfont32) / sizeof(tfont32[0]));
    for (k = 0; k < hz_num; k++) {
        if ((tfont32[k].Index[0] == *s) && (tfont32[k].Index[1] == *(s + 1))) {
            if (st7789_set_window(lcd, x, y, (uint16_t)(x + sizey - 1U), (uint16_t)(y + sizey - 1U)) != ST7789_OK) {
                st7789_end_write(lcd);
                return;
            }
            for (i = 0; i < face_num; i++) {
                for (j = 0; j < 8U; j++) {
                    if (mode == 0U) {
                        if ((tfont32[k].Msk[i] & (uint8_t)(1U << j)) != 0U) {
                            lcd_wr_rgb565(lcd, fc);
                        } else {
                            lcd_wr_rgb565(lcd, bc);
                        }
                        m++;
                        if ((m % sizey) == 0U) {
                            m = 0;
                            break;
                        }
                    } else {
                        if ((tfont32[k].Msk[i] & (uint8_t)(1U << j)) != 0U) {
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
    }
}

void lcd_show_chinese(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey,
                      uint8_t mode) {
    if (lcd == NULL || !st7789_is_initialized(lcd) || s == NULL) {
        return;
    }
    while (*s != 0U) {
        if (sizey == 12U) {
            lcd_show_chinese_12x12(lcd, x, y, s, fc, bc, sizey, mode);
        } else if (sizey == 16U) {
            lcd_show_chinese_16x16(lcd, x, y, s, fc, bc, sizey, mode);
        } else if (sizey == 24U) {
            lcd_show_chinese_24x24(lcd, x, y, s, fc, bc, sizey, mode);
        } else if (sizey == 32U) {
            lcd_show_chinese_32x32(lcd, x, y, s, fc, bc, sizey, mode);
        } else {
            return;
        }
        s += 2;
        x = (uint16_t)(x + sizey);
    }
}

void lcd_show_char(st7789_t *lcd, uint16_t x, uint16_t y, uint8_t num, uint16_t fc, uint16_t bc, uint8_t sizey,
                   uint8_t mode) {
    uint8_t temp;
    uint8_t sizex;
    uint8_t t;
    uint8_t m = 0;
    uint16_t i;
    uint16_t face_num;
    uint16_t x0 = x;
    uint8_t idx;

    if (lcd == NULL || !st7789_is_initialized(lcd)) {
        return;
    }

    sizex = (uint8_t)(sizey / 2U);
    face_num = (uint16_t)((sizex / 8U + ((sizex % 8U) ? 1U : 0U)) * sizey);
    if (num < (uint8_t)' ') {
        return;
    }
    idx = (uint8_t)(num - (uint8_t)' ');
    if (st7789_set_window(lcd, x, y, (uint16_t)(x + sizex - 1U), (uint16_t)(y + sizey - 1U)) != ST7789_OK) {
        st7789_end_write(lcd);
        return;
    }
    for (i = 0; i < face_num; i++) {
        if (sizey == 12U) {
            temp = ascii_1206[idx][i];
        } else if (sizey == 16U) {
            temp = ascii_1608[idx][i];
        } else if (sizey == 24U) {
            temp = ascii_2412[idx][i];
        } else if (sizey == 32U) {
            temp = ascii_3216[idx][i];
        } else {
            st7789_end_write(lcd);
            return;
        }
        for (t = 0; t < 8U; t++) {
            if (mode == 0U) {
                if ((temp & (uint8_t)(1U << t)) != 0U) {
                    lcd_wr_rgb565(lcd, fc);
                } else {
                    lcd_wr_rgb565(lcd, bc);
                }
                m++;
                if ((m % sizex) == 0U) {
                    m = 0;
                    break;
                }
            } else {
                if ((temp & (uint8_t)(1U << t)) != 0U) {
                    lcd_draw_point(lcd, x, y, fc);
                }
                x++;
                if ((x - x0) == sizex) {
                    x = x0;
                    y++;
                    break;
                }
            }
        }
    }
    st7789_end_write(lcd);
}

void lcd_show_string(st7789_t *lcd, uint16_t x, uint16_t y, const uint8_t *p, uint16_t fc, uint16_t bc, uint8_t sizey,
                     uint8_t mode) {
    if (lcd == NULL || !st7789_is_initialized(lcd) || p == NULL) {
        return;
    }
    while (*p != 0U) {
        lcd_show_char(lcd, x, y, *p, fc, bc, sizey, mode);
        x = (uint16_t)(x + (sizey / 2U));
        p++;
    }
}

uint32_t lcd_ui_pow(uint8_t m, uint8_t n) {
    uint32_t result = 1U;
    while (n > 0U) {
        result *= (uint32_t)m;
        n--;
    }
    return result;
}

void lcd_show_int_num(st7789_t *lcd, uint16_t x, uint16_t y, uint16_t num, uint8_t len, uint16_t fc, uint16_t bc,
                      uint8_t sizey) {
    uint8_t t;
    uint8_t temp;
    uint8_t enshow = 0;
    uint8_t sizex = (uint8_t)(sizey / 2U);

    if (lcd == NULL || !st7789_is_initialized(lcd)) {
        return;
    }

    for (t = 0; t < len; t++) {
        temp = (uint8_t)((num / (uint16_t)lcd_ui_pow(10, (uint8_t)(len - t - 1U))) % 10U);
        if ((enshow == 0U) && (t < (uint8_t)(len - 1U))) {
            if (temp == 0U) {
                lcd_show_char(lcd, (uint16_t)(x + t * sizex), y, (uint8_t)' ', fc, bc, sizey, 0U);
                continue;
            }
            enshow = 1U;
        }
        lcd_show_char(lcd, (uint16_t)(x + t * sizex), y, (uint8_t)(temp + 48U), fc, bc, sizey, 0U);
    }
}

void lcd_show_float_num1(st7789_t *lcd, uint16_t x, uint16_t y, float num, uint8_t len, uint16_t fc, uint16_t bc,
                         uint8_t sizey) {
    uint8_t t;
    uint8_t temp;
    uint8_t sizex;
    uint16_t num1;

    if (lcd == NULL || !st7789_is_initialized(lcd)) {
        return;
    }

    sizex = (uint8_t)(sizey / 2U);
    num1 = (uint16_t)(num * 100.0f);
    for (t = 0; t < len; t++) {
        temp = (uint8_t)((num1 / (uint16_t)lcd_ui_pow(10, (uint8_t)(len - t - 1U))) % 10U);
        if (t == (uint8_t)(len - 2U)) {
            lcd_show_char(lcd, (uint16_t)(x + (uint16_t)((len - 2U) * sizex)), y, (uint8_t)'.', fc, bc, sizey, 0U);
            t++;
            len++;
        }
        lcd_show_char(lcd, (uint16_t)(x + t * sizex), y, (uint8_t)(temp + 48U), fc, bc, sizey, 0U);
    }
}

void lcd_show_picture(st7789_t *lcd, uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t *pic) {
    uint16_t row_bytes;

    if (lcd == NULL || !st7789_is_initialized(lcd) || pic == NULL) {
        return;
    }
    row_bytes = (uint16_t)(length * 2U);
    if (row_bytes == 0U) {
        return;
    }
    if (st7789_set_window(lcd, x, y, (uint16_t)(x + length - 1U), (uint16_t)(y + width - 1U)) != ST7789_OK) {
        st7789_end_write(lcd);
        return;
    }
    /* 行优先 RGB565 大端；按多行打包发送，单次不超过 LCD_FAST_FILL_BLK，与 SPI max_transfer_sz 习惯一致。 */
    for (uint16_t j = 0U; j < width;) {
        uint32_t rows_left = (uint32_t)width - (uint32_t)j;
        uint32_t rb        = (uint32_t)row_bytes;
        uint32_t rows_batch;

        rows_batch = LCD_FAST_FILL_BLK / rb;
        if (rows_batch == 0U) {
            rows_batch = 1U;
        }
        if (rows_batch > rows_left) {
            rows_batch = rows_left;
        }
        {
            uint32_t nbytes = rows_batch * rb;
            const uint8_t *p = &pic[(uint32_t)j * rb];

            if (st7789_write_pixel_bytes(lcd, p, nbytes) != ST7789_OK) {
                break;
            }
        }
        j = (uint16_t)(j + (uint16_t)rows_batch);
    }
    st7789_end_write(lcd);
}
