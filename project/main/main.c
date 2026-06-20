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
#include "lcd_video.h"
#include "net_wifi.h"
#include "nvs.h"
#include "battery.h"

/* ---------- 可调参数（业务任务） ---------- */

/**
 * 顶栏文字使用 `lcd_show_char` 的 mode=0（每字格铺背景色再画前景）。
 * mode≠0 时只画前景像素，换数字后旧笔画不会被擦掉，电量/IP 等会叠影。
 * 字格内无法透出图库，若需半透明效果需改图库条带重绘等方案。
 */
#define APP_LCD_STATUS_TEXT_BG (LCD_COLOR_BLACK)

#define APP_MODULES_TASK_STACK_WORDS (8192U)
/** 略低于按键扫描任务，避免长 SPI 传输时饿死短周期人机逻辑。 */
#define APP_MODULES_TASK_PRIORITY (4U)

/** 轮询按键周期（毫秒）；用于图库切换与顶部网络状态刷新。（GPIO0 长按切槽由 `start.c` 处理，此处仅处理单击。） */
#define APP_GALLERY_BUTTON_POLL_MS (50U)

/** 每 N 次轮询刷新一次顶栏网络+电量（50ms × N；勿过小以免 SPI 与残影）。 */
#define APP_LCD_NET_REFRESH_POLLS (100U)

/** 顶栏：左侧网络文案 + 右侧固定 4 字宽电量（分两次 `lcd_show_string`）。电量 `%3u%%`：数字右对齐占 3 格，前置空格由 mode0 擦旧字。 */
static void app_lcd_draw_top_status(st7789_t *lcd)
{
    char              ip[20];
    char              netline[48];
    char              batt[8];
    bool              ip_ok;
    const char       *tag;
    battery_info_t    bi;
    bool_t            bat_ok;
    uint16_t          dsp_w;
    uint16_t          chw;
    uint16_t          x_batt;
    uint16_t          room_px;
    uint16_t          max_net_chars;
    size_t            nl;

    if (!st7789_is_initialized(lcd)) {
        return;
    }

    dsp_w = st7789_display_width(lcd);
    chw   = 8U; /* 16 点阵 ASCII：lcd_show_string 每字 x 步进 sizey/2 */

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

    bat_ok = battery_percent_update();
    if ((bat_ok != FALSE) && (battery_info_read(&bi, NULL) != FALSE)) {
        (void)snprintf(batt, sizeof(batt), "%3u%%", (unsigned int)bi.percent);
    } else {
        (void)snprintf(batt, sizeof(batt), "---%%");
    }

    (void)snprintf(netline, sizeof(netline), "%s  %s", ip, tag);

    /* 电量占屏幕最右 4 字宽，与左侧网络至少隔 1 字宽，网络过长则截断避免叠到电量区 */
    if (dsp_w > (4U + 4U * chw + chw)) {
        x_batt = (uint16_t)(dsp_w - 4U - 4U * chw);
    } else {
        x_batt = 4U;
    }
    room_px = (uint16_t)(x_batt - 4U - chw);
    max_net_chars = room_px / chw;
    if (max_net_chars >= sizeof(netline)) {
        max_net_chars = (uint16_t)(sizeof(netline) - 1U);
    }
    nl = strlen(netline);
    if (nl > (size_t)max_net_chars) {
        netline[max_net_chars] = '\0';
        nl = (size_t)max_net_chars;
    }
    /* 文案变短时若不占满左侧区域，旧字格不会被重画；用空格铺到固定宽度以擦掉残留 */
    for (size_t i = nl; i < (size_t)max_net_chars; i++) {
        netline[i] = ' ';
    }
    netline[max_net_chars] = '\0';

    lcd_show_string(lcd, 4U, 4U, (const uint8_t *)netline, LCD_COLOR_WHITE, APP_LCD_STATUS_TEXT_BG, 16U, 0U);
    lcd_show_string(lcd, x_batt, 4U, (const uint8_t *)batt, LCD_COLOR_WHITE, APP_LCD_STATUS_TEXT_BG, 16U, 0U);
}

static lcd_ui_mode_t s_lcd_ui_mode = LCD_UI_MODE_GALLERY;

static void app_lcd_restore_gallery(st7789_t *lcd)
{
    if (!st7789_is_initialized(lcd) || (sdcard_get_card() == NULL)) {
        return;
    }
    if (lcd_gallery_count() > 0U) {
        (void)lcd_gallery_show_index(lcd, lcd_gallery_current());
    }
    app_lcd_draw_top_status(lcd);
}

static void app_lcd_run_pending_gallery_show(st7789_t *lcd)
{
    char       path[320];
    uint8_t    ix;
    const char *bn;

    if (!lcd_gallery_take_pending_show(path, sizeof(path))) {
        return;
    }
    if (lcd_gallery_show_path(lcd, path) != STATUS_OK) {
        LOG_WARN("app_lcd: gallery show failed %s", path);
        return;
    }
    bn = strrchr(path, '/');
    bn = (bn != NULL) ? (bn + 1) : path;
    ix = lcd_gallery_find_index_by_basename(bn);
    if (ix != LCD_GALLERY_INDEX_NONE) {
        lcd_gallery_set_current_index(ix);
    }
    s_lcd_ui_mode = LCD_UI_MODE_GALLERY;
    app_lcd_draw_top_status(lcd);
}

static void app_lcd_run_pending_video(st7789_t *lcd)
{
    lcd_video_pending_kind_t kind;
    char                     path[320];
    uint8_t                  idx;
    uint32_t                 bench_frames = 0U;

    if (lcd_gallery_peek_pending_show()) {
        return;
    }

    if (!lcd_video_take_pending_play(&kind, path, sizeof(path), &idx, &bench_frames)) {
        return;
    }

    s_lcd_ui_mode = LCD_UI_MODE_VIDEO;
    if (kind == LCD_VIDEO_PENDING_PLAY_INDEX) {
        (void)lcd_video_play_index(lcd, idx);
    } else if (kind == LCD_VIDEO_PENDING_BENCH) {
        (void)lcd_video_benchmark(lcd, path, bench_frames);
    } else {
        (void)lcd_video_play_path(lcd, path);
    }
    s_lcd_ui_mode = LCD_UI_MODE_GALLERY;
    app_lcd_restore_gallery(lcd);
}

static void application_modules_task(void *arg)
{
    (void)arg;

    st7789_t *lcd = BoardSt7789();

    if (st7789_is_initialized(lcd) && (sdcard_get_card() != NULL)) {
        char      boot_name[NVS_LCD_GAL_BOOT_SIZE];
        uint8_t   start_idx = 0U;

        lcd_gallery_rescan();
        (void)lcd_video_scan();
        if (lcd_gallery_count() > 0U) {
            if (nvs_lcd_gallery_boot_name_get(boot_name, sizeof(boot_name)) && (boot_name[0] != '\0')) {
                uint8_t fi = lcd_gallery_find_index_by_basename(boot_name);

                if (fi != LCD_GALLERY_INDEX_NONE) {
                    start_idx = fi;
                }
            }
            (void)lcd_gallery_show_index(lcd, start_idx);
        }
    } else if (sdcard_get_card() == NULL) {
        LOG_WARN("SD gallery: card not mounted, skip scan");
    }
    if (st7789_is_initialized(lcd)) {
        app_lcd_draw_top_status(lcd);
    }

    for (;;) {
        btn_id_e    bid;
        btn_event_e bev;
        static uint16_t s_net_poll;

        lcd = BoardSt7789();

        button_last_event_get(&bid, &bev);
        if ((s_lcd_ui_mode == LCD_UI_MODE_GALLERY) && (bev == BTN_EVENT_SINGLE_CLICK) && (bid == BTN_ID_UP) &&
            st7789_is_initialized(lcd) && (sdcard_get_card() != NULL) && (lcd_gallery_count() > 0U)) {
            button_last_event_clear();
            lcd_gallery_next();
            (void)lcd_gallery_show_index(lcd, lcd_gallery_current());
            app_lcd_draw_top_status(lcd);
        } else if ((s_lcd_ui_mode == LCD_UI_MODE_VIDEO) && (bev == BTN_EVENT_SINGLE_CLICK) && (bid == BTN_ID_UP)) {
            button_last_event_clear();
            lcd_video_request_stop();
        }

        app_lcd_run_pending_video(lcd);

        if (lcd_gallery_peek_pending_show()) {
            if (lcd_video_is_playing()) {
                lcd_video_request_stop();
            } else {
                app_lcd_run_pending_gallery_show(lcd);
            }
        }

        if (s_lcd_ui_mode == LCD_UI_MODE_GALLERY) {
            s_net_poll++;
            if (s_net_poll >= APP_LCD_NET_REFRESH_POLLS) {
                s_net_poll = 0U;
                app_lcd_draw_top_status(lcd);
            }
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
