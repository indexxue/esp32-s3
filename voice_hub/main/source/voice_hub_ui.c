#include "voice_hub_ui.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "voice_hub_config.h"
#include "board.h"
#include "lcd.h"
#include "net_wifi.h"

#define VOICE_HUB_UI_IP_OPAQUE (0U)

static char              s_status_line[48];
static char              s_ip_line[20];
static bool_t            s_ip_overlay_dirty = TRUE;
static SemaphoreHandle_t s_lcd_mtx;

static status_t voice_hub_ui_lcd_mtx_init(void)
{
    if (s_lcd_mtx != NULL) {
        return STATUS_OK;
    }
    s_lcd_mtx = xSemaphoreCreateMutex();
    if (s_lcd_mtx == NULL) {
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

bool_t voice_hub_ui_lcd_lock(uint32_t timeout_ms)
{
    if (voice_hub_ui_lcd_mtx_init() != STATUS_OK) {
        return FALSE;
    }
    return (xSemaphoreTake(s_lcd_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) ? TRUE : FALSE;
}

void voice_hub_ui_lcd_unlock(void)
{
    if (s_lcd_mtx != NULL) {
        (void)xSemaphoreGive(s_lcd_mtx);
    }
}

static uint16_t voice_hub_ui_text_width_12(const char *text)
{
    size_t n;

    if (text == NULL) {
        return 0U;
    }
    n = strlen(text);
    return (uint16_t)(n * (VOICE_HUB_UI_IP_FONT_SIZE / 2U));
}

void voice_hub_ui_refresh_ip(void)
{
    char next[sizeof(s_ip_line)];

    if (!net_wifi_format_ipv4_for_display(next, sizeof(next))) {
        (void)strncpy(next, "no ip", sizeof(next) - 1U);
        next[sizeof(next) - 1U] = '\0';
    }

    if (strcmp(next, s_ip_line) != 0) {
        (void)strncpy(s_ip_line, next, sizeof(s_ip_line) - 1U);
        s_ip_line[sizeof(s_ip_line) - 1U] = '\0';
        s_ip_overlay_dirty = TRUE;
    }
}

const char *voice_hub_ui_ip_line(void)
{
    if (s_ip_line[0] == '\0') {
        voice_hub_ui_refresh_ip();
    }
    return s_ip_line;
}

static void voice_hub_ui_draw_ip_overlay_nolock(st7789_t *lcd)
{
    uint16_t lcd_w;
    uint16_t text_w;
    uint16_t x;
    const char *ip;

    if ((lcd == NULL) || !st7789_is_initialized(lcd)) {
        return;
    }

    ip     = voice_hub_ui_ip_line();
    lcd_w  = st7789_display_width(lcd);
    text_w = voice_hub_ui_text_width_12(ip);
    if (text_w == 0U) {
        return;
    }

    if (lcd_w <= text_w + VOICE_HUB_UI_IP_MARGIN_X) {
        x = 0U;
    } else {
        x = (uint16_t)(lcd_w - text_w - VOICE_HUB_UI_IP_MARGIN_X);
    }

    lcd_fill(lcd, 0U, 0U, lcd_w, VOICE_HUB_UI_IP_BAND_H, LCD_COLOR_BLACK);
    lcd_show_string(lcd,
                    x,
                    VOICE_HUB_UI_IP_MARGIN_Y,
                    (const uint8_t *)ip,
                    LCD_COLOR_WHITE,
                    LCD_COLOR_BLACK,
                    VOICE_HUB_UI_IP_FONT_SIZE,
                    VOICE_HUB_UI_IP_OPAQUE);
    s_ip_overlay_dirty = FALSE;
}

void voice_hub_ui_draw_ip_overlay(st7789_t *lcd)
{
    if (!voice_hub_ui_lcd_lock(500U)) {
        return;
    }
    voice_hub_ui_draw_ip_overlay_nolock(lcd);
    voice_hub_ui_lcd_unlock();
}

void voice_hub_ui_redraw_ip_if_dirty(st7789_t *lcd)
{
    if (!s_ip_overlay_dirty) {
        return;
    }
    if (!voice_hub_ui_lcd_lock(500U)) {
        return;
    }
    if (s_ip_overlay_dirty) {
        voice_hub_ui_draw_ip_overlay_nolock(lcd);
    }
    voice_hub_ui_lcd_unlock();
}

void voice_hub_ui_on_ipv4_changed(void)
{
    st7789_t *lcd = BoardSt7789();

    voice_hub_ui_refresh_ip();
    if ((lcd != NULL) && st7789_is_initialized(lcd)) {
        voice_hub_ui_redraw_ip_if_dirty(lcd);
    }
}

status_t voice_hub_ui_init(st7789_t *lcd)
{
    if ((lcd == NULL) || !st7789_is_initialized(lcd)) {
        return STATUS_INVALID_ARG;
    }
    if (voice_hub_ui_lcd_mtx_init() != STATUS_OK) {
        return STATUS_FAIL;
    }
    s_status_line[0] = '\0';
    s_ip_overlay_dirty = TRUE;
    voice_hub_ui_refresh_ip();
    if (!voice_hub_ui_lcd_lock(1000U)) {
        return STATUS_FAIL;
    }
    voice_hub_ui_draw_idle_nolock(lcd);
    voice_hub_ui_lcd_unlock();
    return STATUS_OK;
}

void voice_hub_ui_draw_idle_nolock(st7789_t *lcd)
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
    lcd_show_string(lcd, 4U, 4U, (const uint8_t *)"voice_hub", LCD_COLOR_WHITE, LCD_COLOR_BLACK, 16U, VOICE_HUB_UI_IP_OPAQUE);
    if (s_status_line[0] != '\0') {
        lcd_show_string(lcd, 4U, 24U, (const uint8_t *)s_status_line, LCD_COLOR_CYAN, LCD_COLOR_BLACK, 16U, VOICE_HUB_UI_IP_OPAQUE);
    }
    voice_hub_ui_draw_ip_overlay_nolock(lcd);
}

void voice_hub_ui_draw_idle(st7789_t *lcd)
{
    if (!voice_hub_ui_lcd_lock(1000U)) {
        return;
    }
    voice_hub_ui_draw_idle_nolock(lcd);
    voice_hub_ui_lcd_unlock();
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
