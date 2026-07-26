#include "camera_ui.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board.h"
#include "lcd.h"

static char              s_status_line[48];
static SemaphoreHandle_t s_lcd_mtx;
static volatile bool_t   s_lcd_open = TRUE;

static void camera_ui_rgb565_be_put(uint8_t *buf, uint16_t width, uint16_t height,
                                    uint16_t x, uint16_t y, uint16_t color)
{
    uint8_t *p;

    if ((buf == NULL) || (x >= width) || (y >= height)) {
        return;
    }
    p = buf + ((((uint32_t)y * (uint32_t)width) + (uint32_t)x) * 2U);
    p[0] = (uint8_t)(color >> 8);
    p[1] = (uint8_t)(color & 0xFFU);
}

static void camera_ui_rgb565_be_hline(uint8_t *buf, uint16_t width, uint16_t height,
                                      uint16_t x0, uint16_t x1, uint16_t y, uint16_t color)
{
    uint16_t x;

    if (y >= height) {
        return;
    }
    if (x0 > x1) {
        uint16_t t = x0;
        x0 = x1;
        x1 = t;
    }
    if (x0 >= width) {
        return;
    }
    if (x1 >= width) {
        x1 = (uint16_t)(width - 1U);
    }
    for (x = x0; x <= x1; x++) {
        camera_ui_rgb565_be_put(buf, width, height, x, y, color);
    }
}

static void camera_ui_rgb565_be_vline(uint8_t *buf, uint16_t width, uint16_t height,
                                      uint16_t x, uint16_t y0, uint16_t y1, uint16_t color)
{
    uint16_t y;

    if (x >= width) {
        return;
    }
    if (y0 > y1) {
        uint16_t t = y0;
        y0 = y1;
        y1 = t;
    }
    if (y0 >= height) {
        return;
    }
    if (y1 >= height) {
        y1 = (uint16_t)(height - 1U);
    }
    for (y = y0; y <= y1; y++) {
        camera_ui_rgb565_be_put(buf, width, height, x, y, color);
    }
}

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
    if (s_lcd_open == FALSE) {
        return;
    }
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

void camera_ui_draw_boxes_rgb565(uint8_t *rgb565_be,
                                 uint16_t width,
                                 uint16_t height,
                                 const uint16_t *boxes,
                                 uint8_t count,
                                 uint16_t cam_w,
                                 uint16_t cam_h)
{
    uint8_t i;

    if ((rgb565_be == NULL) || (boxes == NULL) || (count == 0U) || (width == 0U) || (height == 0U)) {
        return;
    }
    if (cam_w == 0U) {
        cam_w = width;
    }
    if (cam_h == 0U) {
        cam_h = height;
    }

    for (i = 0U; i < count; i++) {
        const uint16_t *b = boxes + ((uint32_t)i * 5U);
        uint16_t bx = b[0];
        uint16_t by = b[1];
        uint16_t bw = b[2];
        uint16_t bh = b[3];
        uint16_t color = b[4];
        uint16_t x2;
        uint16_t y2;

        if ((bw == 0U) || (bh == 0U)) {
            continue;
        }
        if ((cam_w != width) || (cam_h != height)) {
            bx = (uint16_t)((uint32_t)bx * (uint32_t)width / (uint32_t)cam_w);
            by = (uint16_t)((uint32_t)by * (uint32_t)height / (uint32_t)cam_h);
            bw = (uint16_t)((uint32_t)bw * (uint32_t)width / (uint32_t)cam_w);
            bh = (uint16_t)((uint32_t)bh * (uint32_t)height / (uint32_t)cam_h);
            if (bw == 0U) {
                bw = 1U;
            }
            if (bh == 0U) {
                bh = 1U;
            }
        }
        if (bx >= width) {
            continue;
        }
        if (by >= height) {
            continue;
        }
        x2 = (uint16_t)(bx + bw - 1U);
        y2 = (uint16_t)(by + bh - 1U);
        if (x2 >= width) {
            x2 = (uint16_t)(width - 1U);
        }
        if (y2 >= height) {
            y2 = (uint16_t)(height - 1U);
        }

        camera_ui_rgb565_be_hline(rgb565_be, width, height, bx, x2, by, color);
        camera_ui_rgb565_be_hline(rgb565_be, width, height, bx, x2, y2, color);
        camera_ui_rgb565_be_vline(rgb565_be, width, height, bx, by, y2, color);
        camera_ui_rgb565_be_vline(rgb565_be, width, height, x2, by, y2, color);
    }
}

status_t camera_ui_lcd_close(void)
{
    st7789_t *lcd = BoardSt7789();

    s_lcd_open = FALSE;
    if ((lcd == NULL) || !st7789_is_initialized(lcd)) {
        return STATUS_OK;
    }
    if (camera_ui_lcd_lock(200U)) {
        uint16_t w = st7789_display_width(lcd);
        uint16_t h = st7789_display_height(lcd);
        lcd_fill(lcd, 0U, 0U, w, h, LCD_COLOR_BLACK);
        camera_ui_lcd_unlock();
    }
    (void)st7789_set_backlight(lcd, false);
    return STATUS_OK;
}

status_t camera_ui_lcd_open(void)
{
    st7789_t *lcd = BoardSt7789();

    s_lcd_open = TRUE;
    if ((lcd != NULL) && st7789_is_initialized(lcd)) {
        (void)st7789_set_backlight(lcd, true);
        camera_ui_draw_status_band(lcd);
    }
    return STATUS_OK;
}

bool_t camera_ui_lcd_is_open(void)
{
    return s_lcd_open;
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
    s_lcd_open = TRUE;
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
    if ((lcd != NULL) && st7789_is_initialized(lcd) && (s_lcd_open != FALSE)) {
        camera_ui_draw_status_band(lcd);
    }
}
