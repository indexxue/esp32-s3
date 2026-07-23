#include "camera_ui.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board.h"
#include "lcd.h"

static char              s_status_line[48];
static SemaphoreHandle_t s_lcd_mtx;

static status_t camera_ui_lcd_mtx_init(void)
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

bool_t camera_ui_lcd_lock(uint32_t timeout_ms)
{
    if (camera_ui_lcd_mtx_init() != STATUS_OK) {
        return FALSE;
    }
    return (xSemaphoreTake(s_lcd_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) ? TRUE : FALSE;
}

void camera_ui_lcd_unlock(void)
{
    if (s_lcd_mtx != NULL) {
        (void)xSemaphoreGive(s_lcd_mtx);
    }
}

static void camera_ui_draw_status_band_nolock(st7789_t *lcd)
{
    uint16_t lcd_w;

    if ((lcd == NULL) || !st7789_is_initialized(lcd)) {
        return;
    }

    lcd_w = st7789_display_width(lcd);
    lcd_fill(lcd, 0U, 0U, lcd_w, CAMERA_UI_IP_BAND_H, LCD_COLOR_BLACK);
    if (s_status_line[0] != '\0') {
        lcd_show_string(lcd,
                        2U,
                        1U,
                        (const uint8_t *)s_status_line,
                        LCD_COLOR_CYAN,
                        LCD_COLOR_BLACK,
                        12U,
                        0U);
    }
}

void camera_ui_draw_status_band(st7789_t *lcd)
{
    if (!camera_ui_lcd_lock(100U)) {
        return;
    }
    camera_ui_draw_status_band_nolock(lcd);
    camera_ui_lcd_unlock();
}

void camera_ui_draw_detection_boxes(st7789_t *lcd,
                                     const uint16_t *boxes,
                                     uint8_t count,
                                     uint16_t cam_w,
                                     uint16_t cam_h)
{
    uint16_t lcd_w;
    uint16_t lcd_h;
    uint16_t view_h;
    uint16_t view_top;
    uint16_t crop_y;
    uint8_t  i;

    if ((lcd == NULL) || !st7789_is_initialized(lcd) || (boxes == NULL) || (count == 0U)) {
        return;
    }

    lcd_w = st7789_display_width(lcd);
    lcd_h = st7789_display_height(lcd);
    view_top = CAMERA_UI_IP_BAND_H;
    if (lcd_h <= view_top) {
        return;
    }
    view_h = (uint16_t)(lcd_h - view_top);

    /* camera 240×240 居中裁剪到 LCD 可视区 */
    if (cam_h > view_h) {
        crop_y = (uint16_t)((cam_h - view_h) / 2U);
    } else {
        crop_y = 0U;
    }

    if (!camera_ui_lcd_lock(100U)) {
        return;
    }

    for (i = 0U; i < count; i++) {
        /* boxes: [x, y, w, h, color_565] 每框 5 个 uint16 */
        const uint16_t *b = boxes + ((uint32_t)i * 5U);
        uint16_t bx = b[0];
        int16_t  by = (int16_t)b[1] - (int16_t)crop_y;
        uint16_t bw = b[2];
        uint16_t bh = b[3];
        uint16_t color = b[4];

        /* 缩放到 LCD 宽度 */
        if (cam_w > 0U && cam_w != lcd_w) {
            bx = (uint16_t)((uint32_t)bx * (uint32_t)lcd_w / (uint32_t)cam_w);
            bw = (uint16_t)((uint32_t)bw * (uint32_t)lcd_w / (uint32_t)cam_w);
        }
        if (by < 0) {
            by = 0;
        }
        if ((uint16_t)by + bh > view_h) {
            bh = (uint16_t)(view_h - (uint16_t)by);
        }

        lcd_draw_rectangle(lcd, bx, (uint16_t)((uint16_t)by + view_top),
                           (uint16_t)(bx + bw - 1U),
                           (uint16_t)((uint16_t)by + view_top + bh - 1U),
                           color);
    }

    camera_ui_lcd_unlock();
}

status_t camera_ui_init(st7789_t *lcd)
{
    if ((lcd == NULL) || !st7789_is_initialized(lcd)) {
        return STATUS_INVALID_ARG;
    }
    if (camera_ui_lcd_mtx_init() != STATUS_OK) {
        return STATUS_FAIL;
    }
    s_status_line[0] = '\0';
    if (!camera_ui_lcd_lock(1000U)) {
        return STATUS_FAIL;
    }
    camera_ui_draw_idle_nolock(lcd);
    camera_ui_lcd_unlock();
    return STATUS_OK;
}

void camera_ui_draw_idle_nolock(st7789_t *lcd)
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
    lcd_show_string(lcd, 4U, 4U, (const uint8_t *)"camera", LCD_COLOR_WHITE, LCD_COLOR_BLACK, 16U, 0U);
    camera_ui_draw_status_band_nolock(lcd);
}

void camera_ui_draw_idle(st7789_t *lcd)
{
    if (!camera_ui_lcd_lock(1000U)) {
        return;
    }
    camera_ui_draw_idle_nolock(lcd);
    camera_ui_lcd_unlock();
}

void camera_ui_set_status_line(st7789_t *lcd, const char *line)
{
    if (line == NULL) {
        s_status_line[0] = '\0';
    } else {
        (void)strncpy(s_status_line, line, sizeof(s_status_line) - 1U);
        s_status_line[sizeof(s_status_line) - 1U] = '\0';
    }
    if ((lcd != NULL) && st7789_is_initialized(lcd)) {
        camera_ui_draw_status_band(lcd);
    }
}
