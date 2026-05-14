/*
 * @Author: indexxue 2308039918@qq.com
 * @Date: 2026-04-30 21:58:22
 * @LastEditors: indexxue 2308039918@qq.com
 * @LastEditTime: 2026-05-02 10:16:34
 * @FilePath: \ESP32-S3\project\main\main.c
 * @Description: 应用入口 `app_main`；业务任务（LCD/SD 等）见本文件 `application_start_modules_task`。GPIO0 长按切换 OTA 启动槽在 `start.c` 的按键回调中处理。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#include "type.h"

#include "log.h"
#include "start.h"
#include "board.h"
#include "sdcard.h"
#include "lcd.h"
#include "button.h"
#include "lcd_gallery.h"
#include "net_wifi.h"

/* ---------- 可调参数（业务任务） ---------- */

/** `lcd_show_char` 非 0：只画前景像素，不铺背景色（叠在图库画面上）。 */
#define APP_LCD_TEXT_OVERLAY (1U)

#define APP_MODULES_TASK_STACK_WORDS (4096U)
/** 略低于按键扫描任务，避免长 SPI 传输时饿死短周期人机逻辑。 */
#define APP_MODULES_TASK_PRIORITY (4U)

/** 轮询按键周期（毫秒）；用于图库切换与顶部网络状态刷新。（GPIO0 长按切槽由 `start.c` 处理，此处仅处理单击。） */
#define APP_GALLERY_BUTTON_POLL_MS (50U)

/** 每 N 次轮询刷新一次顶部网络信息（50ms × N；勿过小以免 SPI 与残影）。 */
#define APP_LCD_NET_REFRESH_POLLS (100U)

static void app_lcd_draw_net_status(st7789_t *lcd)
{
    char     ip[20];
    char     line[40];
    bool     ip_ok;
    const char *tag;

    if (!st7789_is_initialized(lcd)) {
        return;
    }
    ip_ok = net_wifi_format_ipv4_for_display(ip, sizeof(ip));

    if (!net_wifi_is_started()) {
        tag = "off";
    } else if (ip_ok && (net_wifi_get_mode() == NET_WIFI_MODE_STA)) {
        tag = "STA OK";
    } else if (ip_ok && (net_wifi_get_mode() == NET_WIFI_MODE_SOFTAP)) {
        tag = "AP OK";
    } else if (net_wifi_get_mode() == NET_WIFI_MODE_STA) {
        tag = "no IP";
    } else {
        tag = "--";
    }
    (void)snprintf(line, sizeof(line), "%s  %s", ip, tag);

    /* 无整行底色；字形不铺底（mode=1），叠在图库像素上。 */
    lcd_show_string(lcd, 4U, 4U, (const uint8_t *)line, LCD_COLOR_WHITE, 0U, 16U, APP_LCD_TEXT_OVERLAY);
}

static void application_modules_task(void *arg)
{
    (void)arg;

    st7789_t *lcd = BoardSt7789();

    if (st7789_is_initialized(lcd) && (sdcard_get_card() != NULL)) {
        lcd_gallery_rescan();
        if (lcd_gallery_count() > 0U) {
            (void)lcd_gallery_show_index(lcd, 0U);
        }
    } else if (sdcard_get_card() == NULL) {
        LOG_WARN("SD gallery: card not mounted, skip scan");
    }
    if (st7789_is_initialized(lcd)) {
        app_lcd_draw_net_status(lcd);
    }

    for (;;) {
        btn_id_e    bid;
        btn_event_e bev;
        static uint16_t s_net_poll;

        lcd = BoardSt7789();

        button_last_event_get(&bid, &bev);
        if ((bev == BTN_EVENT_SINGLE_CLICK) && (bid == BTN_ID_GPIO0) && st7789_is_initialized(lcd) &&
            (sdcard_get_card() != NULL) && (lcd_gallery_count() > 0U)) {
            button_last_event_clear();
            lcd_gallery_next();
            (void)lcd_gallery_show_index(lcd, lcd_gallery_current());
            app_lcd_draw_net_status(lcd);
        }

        s_net_poll++;
        if (s_net_poll >= APP_LCD_NET_REFRESH_POLLS) {
            s_net_poll = 0U;
            app_lcd_draw_net_status(lcd);
        }

        vTaskDelay(pdMS_TO_TICKS(APP_GALLERY_BUTTON_POLL_MS));
    }
}

status_t application_start_modules_task(void)
{
    if (xTaskCreate(application_modules_task, "app_mod", APP_MODULES_TASK_STACK_WORDS, NULL, APP_MODULES_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

void app_main(void)
{
    status_t err = app_entry();

    if (err != STATUS_OK) {
        LOG_ERROR("application startup failed: %s", status_to_str(err));
    }
}
