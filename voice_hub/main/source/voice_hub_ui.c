#include "voice_hub_ui.h"

#include <string.h>

#include "lcd.h"

static char s_status_line[48];

status_t voice_hub_ui_init(st7789_t *lcd)
{
    if ((lcd == NULL) || !st7789_is_initialized(lcd)) {
        return STATUS_INVALID_ARG;
    }
    s_status_line[0] = '\0';
    voice_hub_ui_draw_idle(lcd);
    return STATUS_OK;
}

void voice_hub_ui_draw_idle(st7789_t *lcd)
{
    uint16_t w;
    uint16_t h;

    if ((lcd == NULL) || !st7789_is_initialized(lcd)) {
        return;
    }

    w = st7789_display_width(lcd);
    h = st7789_display_height(lcd);
    lcd_fill(lcd, 0U, 0U, w, h, LCD_COLOR_BLACK);
    lcd_draw_rectangle(lcd, 0U, 0U, (uint16_t)(w - 1U), (uint16_t)(h - 1U), LCD_COLOR_GREEN);
    lcd_show_string(lcd, 4U, 4U, (const uint8_t *)"voice_hub", LCD_COLOR_WHITE, LCD_COLOR_BLACK, 16U, 0U);
    if (s_status_line[0] != '\0') {
        lcd_show_string(lcd, 4U, 24U, (const uint8_t *)s_status_line, LCD_COLOR_CYAN, LCD_COLOR_BLACK, 16U, 0U);
    }
}

void voice_hub_ui_set_status_line(st7789_t *lcd, const char *line)
{
    if (line == NULL) {
        s_status_line[0] = '\0';
    } else {
        (void)strncpy(s_status_line, line, sizeof(s_status_line) - 1U);
        s_status_line[sizeof(s_status_line) - 1U] = '\0';
    }
    voice_hub_ui_draw_idle(lcd);
}

void voice_hub_ui_set_intercom_active(st7789_t *lcd, bool_t active)
{
    voice_hub_ui_set_status_line(lcd, active ? "intercom ON" : "intercom off");
}
