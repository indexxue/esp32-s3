/**
 * @file ui.c
 * @brief LVGL port: GC9A01 flush, IT7259, pet_view; optional debug overlay.
 */

#include "ui.h"

#include "board.h"
#include "gc9a01.h"
#include "i2c.h"
#include "it7259.h"
#include "led_scene.h"
#include "log.h"
#include "nvs.h"
#include "pet_core.h"
#include "pet_fs.h"
#include "pet_res.h"
#include "pet_sfx.h"
#include "pet_view.h"
#include "sd_cfg.h"
#include "qmi8658a.h"
#include "type.h"
#include "agent.h"
#include "wake.h"

#if DESKTOP_PET_ENABLE_DEBUG_UI
#include "audio.h"
#include "net_wifi.h"
#endif

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define UI_HOR_RES (240)
#define UI_VER_RES (240)
#define UI_BUF_LINES (40)
#define UI_TASK_STACK_WORDS (8192U)
#define UI_TASK_PRIORITY (4U)
#define UI_TICK_PERIOD_MS (1U)
#define UI_GESTURE_PERIOD_MS (100U)
#define UI_ACCEL_LSB_PER_G (16384.0f)
#define UI_GYRO_LSB_PER_DPS (16.0f)
#define UI_RAD2DEG (57.2957795f)
#define UI_SHAKE_G (1.85f)
#define UI_SHAKE_HITS (3U)
#define UI_SHAKE_COOLDOWN_MS (2000U)
#define UI_FLIP_G (-0.55f)
#define UI_FLIP_HITS (10U)
#define UI_CALIB_POINTS (4U)
#define UI_CALIB_ROUNDS (3U)
/* 相对产品 UI：Needs=上、Dock=下；十字略内收便于点准 */
#define UI_CALIB_EDGE (36)
#define UI_CALIB_MARK (32)
#define UI_CALIB_DOT (10)
#define UI_CALIB_TIMER_MS (20U)
#define UI_CALIB_MIN_SAMPLES (1U)
#define UI_CALIB_MAX_SAMPLES (24U)
#define UI_CALIB_END_MS (700U)
#define UI_CALIB_MIN_SPAN (90)
#define UI_CALIB_FIT_MAX_RESID (22)
#define UI_CALIB_EVT_NONE (0U)
#define UI_CALIB_EVT_RELEASE (1U)
#define UI_CALIB_REQ_NONE (0)
#define UI_CALIB_REQ_START (1)
#define UI_CALIB_REQ_CANCEL (2)
#define UI_CALIB_REQ_RESET (3)

#define UI_CALIB_IDX_TOP (0U)   /* Needs / 上 */
#define UI_CALIB_IDX_RIGHT (1U)
#define UI_CALIB_IDX_BOT (2U)   /* Dock / 下 */
#define UI_CALIB_IDX_LEFT (3U)
#define UI_CHAT_CAPTION_MAX (160)
#define UI_BLANK_DEFAULT_S (60U)

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static uint8_t *s_buf1;
static uint8_t *s_buf2;
static esp_timer_handle_t s_tick_timer;
static bool s_started;
static volatile bool s_display_blank;
static TickType_t s_blank_last_activity;
static uint32_t s_blank_timeout_ms; /* 0 = disabled */
#if DESKTOP_PET_ENABLE_DEBUG_UI
static lv_obj_t *s_debug;
static lv_obj_t *s_lbl_roll;
static lv_obj_t *s_lbl_pitch;
static lv_obj_t *s_lbl_yaw;
static lv_obj_t *s_lbl_rec;
static lv_obj_t *s_lbl_ip;
static lv_obj_t *s_btn_rec_lbl;
static lv_obj_t *s_btn_talk_lbl;
static lv_obj_t *s_btn_conn_lbl;
static float s_yaw_deg;
static volatile int s_rec_req;
static volatile int s_play_req;
static volatile int s_debug_req;
static bool s_debug_on;
static bool s_play_shown;
#endif
static uint8_t s_shake_hits;
static uint8_t s_flip_hits;
static uint32_t s_shake_cool_ms;
static desktop_pet_agent_state_t s_chat_agent_prev;
static char s_chat_cap_pending[UI_CHAT_CAPTION_MAX];
static volatile uint8_t s_chat_cap_pending_kind; /* 0=none 1=stt 2=tts 3=llm */
static portMUX_TYPE s_chat_cap_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_chat_reconnect_cool; /* *100ms；WS 意外掉线后冷却再听 */

static void *ui_alloc_buf(size_t nbytes)
{
    void *p = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (p == NULL) {
        p = heap_caps_malloc(nbytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void *ui_alloc_psram(size_t nbytes)
{
    return ui_alloc_buf(nbytes);
}

static void ui_free_psram(void *p)
{
    heap_caps_free(p);
}

#if DESKTOP_PET_ENABLE_TOUCH
static it7259_t s_touch;
static bool s_touch_ok;
static int16_t s_last_x;
static int16_t s_last_y;
static int16_t s_raw_x;
static int16_t s_raw_y;
static bool s_last_pressed;
static nvs_touch_calib_t s_calib;
static bool s_calib_apply;
static volatile int s_calib_req;
static volatile uint8_t s_calib_step;
static volatile bool s_calib_running;
static lv_obj_t *s_calib_layer;
static lv_obj_t *s_calib_mark;
static lv_obj_t *s_calib_lbl;
static lv_obj_t *s_calib_orient_lbl[5];
static lv_timer_t *s_calib_timer;
static uint8_t s_calib_st;
static uint8_t s_calib_idx;
static uint8_t s_calib_round;
static uint8_t s_calib_nsamp;
static bool s_calib_orient_tap;
static volatile uint8_t s_calib_evt;
static int32_t s_calib_sum_x;
static int32_t s_calib_sum_y;
static int32_t s_calib_end_ms;
static int16_t s_calib_raw_x[UI_CALIB_POINTS];
static int16_t s_calib_raw_y[UI_CALIB_POINTS];
static int16_t s_calib_samp_x[UI_CALIB_POINTS][UI_CALIB_ROUNDS];
static int16_t s_calib_samp_y[UI_CALIB_POINTS][UI_CALIB_ROUNDS];
/* 0=上 1=右 2=下 3=左 */
static const int16_t s_calib_tx[UI_CALIB_POINTS] = {
    (int16_t)(UI_HOR_RES / 2), (int16_t)(UI_HOR_RES - UI_CALIB_EDGE), (int16_t)(UI_HOR_RES / 2),
    UI_CALIB_EDGE
};
static const int16_t s_calib_ty[UI_CALIB_POINTS] = {
    UI_CALIB_EDGE, (int16_t)(UI_VER_RES / 2), (int16_t)(UI_VER_RES - UI_CALIB_EDGE),
    (int16_t)(UI_VER_RES / 2)
};
static const char *const s_calib_dir[UI_CALIB_POINTS] = {"Needs", "Right", "Dock", "Left"};
#endif

static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    gc9a01_t *lcd = BoardGc9a01();
    uint32_t w;
    uint32_t h;
    uint32_t px_count;

    if ((lcd == NULL) || !gc9a01_is_initialized(lcd) || (area == NULL) || (px_map == NULL)) {
        lv_display_flush_ready(disp);
        return;
    }
    if (s_display_blank) {
        lv_display_flush_ready(disp);
        return;
    }

    w = (uint32_t)(area->x2 - area->x1 + 1);
    h = (uint32_t)(area->y2 - area->y1 + 1);
    px_count = w * h;

    if (gc9a01_set_window(lcd, (uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)area->x2,
                          (uint16_t)area->y2) != GC9A01_OK) {
        lv_display_flush_ready(disp);
        return;
    }

    (void)gc9a01_write_pixels(lcd, (const uint16_t *)px_map, px_count);
    gc9a01_end_write(lcd);
    lv_display_flush_ready(disp);
}

bool ui_display_is_blank(void)
{
    return s_display_blank;
}

void ui_display_note_activity(void)
{
    s_blank_last_activity = xTaskGetTickCount();
}

void ui_display_blank_set(bool blank)
{
    gc9a01_t *lcd;

    if (blank == s_display_blank) {
        if (!blank) {
            ui_display_note_activity();
        }
        return;
    }

    lcd = BoardGc9a01();
    s_display_blank = blank;
    if (lcd != NULL) {
        (void)gc9a01_set_backlight(lcd, !blank);
    }
    if (blank) {
        desktop_pet_wake_arm_for_blank();
        LOG_INFO("ui: display blank");
    } else {
        ui_display_note_activity();
        LOG_INFO("ui: display lit");
    }
}

static bool ui_blank_allowed(void)
{
    desktop_pet_agent_state_t st;

    if (s_blank_timeout_ms == 0U) {
        return false;
    }
    if (desktop_pet_ui_touch_calib_is_running()) {
        return false;
    }
    if (desktop_pet_wake_is_busy()) {
        return false;
    }
    if (desktop_pet_agent_is_listen_active()) {
        return false;
    }
    st = desktop_pet_agent_get_state();
    if ((st == DESKTOP_PET_AGENT_STATE_LISTENING) || (st == DESKTOP_PET_AGENT_STATE_SPEAKING) ||
        (st == DESKTOP_PET_AGENT_STATE_CONNECTING)) {
        return false;
    }
    return true;
}

static void ui_blank_poll(void)
{
    TickType_t now;

    if (s_display_blank || !ui_blank_allowed()) {
        if (!s_display_blank && !ui_blank_allowed()) {
            /* Keep timer fresh while listen/speak so we don't blank immediately after. */
            ui_display_note_activity();
        }
        return;
    }
    now = xTaskGetTickCount();
    if ((now - s_blank_last_activity) >= pdMS_TO_TICKS(s_blank_timeout_ms)) {
        ui_display_blank_set(true);
    }
}

static void ui_blank_read_cfg(void)
{
    const char *v = sd_cfg_get("screen_blank_s");
    unsigned sec = UI_BLANK_DEFAULT_S;

    if ((v != NULL) && (v[0] != '\0')) {
        sec = (unsigned)strtoul(v, NULL, 10);
    }
    if (sec == 0U) {
        s_blank_timeout_ms = 0U;
        LOG_INFO("ui: screen blank disabled");
    } else {
        if (sec > 3600U) {
            sec = 3600U;
        }
        s_blank_timeout_ms = sec * 1000U;
        LOG_INFO("ui: screen blank after %us", sec);
    }
}

static void ui_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(UI_TICK_PERIOD_MS);
}

#if DESKTOP_PET_ENABLE_TOUCH
static int ui_touch_i2c_write_read(uint8_t addr7,
                                   const uint8_t *write_data,
                                   uint16_t write_len,
                                   uint8_t *read_data,
                                   uint16_t read_len)
{
    if (I2cWriteRead((s32_t)BOARD_DESKTOP_PET_TOUCH_I2C_PORT,
                     (u16_t)addr7,
                     write_data,
                     (usize_t)write_len,
                     read_data,
                     (usize_t)read_len) != TRUE) {
        return -1;
    }
    return 0;
}

enum {
    UI_CALIB_ST_IDLE = 0,
    UI_CALIB_ST_ORIENT,
    UI_CALIB_ST_WAIT_UP,
    UI_CALIB_ST_WAIT_DOWN,
    UI_CALIB_ST_HOLD,
    UI_CALIB_ST_END,
};

static void ui_calib_add_sample(int16_t x, int16_t y);
static void ui_calib_on_press_edge(void);
static void ui_calib_on_release_edge(void);
static void ui_calib_set_mark(uint8_t idx);
static void ui_calib_enter_points(void);

static int16_t ui_calib_map_axis(int16_t v, int32_t a_q16, int32_t b_q16, int16_t maxv)
{
    int32_t out;

    out = (int32_t)(((int64_t)a_q16 * (int64_t)v + (int64_t)b_q16) >> 16);
    if (out < 0) {
        out = 0;
    }
    if (out > (int32_t)maxv) {
        out = (int32_t)maxv;
    }
    return (int16_t)out;
}

static void ui_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    it7259_point_t pt;
    it7259_status_t st;
    static bool s_logged_press;

    (void)indev;
    if (!s_touch_ok || (data == NULL)) {
        if (data != NULL) {
            data->state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }

    st = it7259_read_point(&s_touch, &pt);
    if (st == IT7259_OK) {
        bool was_pressed = s_last_pressed;

        s_last_pressed = pt.pressed;
        s_raw_x = pt.x;
        s_raw_y = pt.y;
        if (s_calib.valid && s_calib_apply) {
            s_last_x = ui_calib_map_axis(pt.x, s_calib.ax_q16, s_calib.bx_q16,
                                        (int16_t)(UI_HOR_RES - 1));
            s_last_y = ui_calib_map_axis(pt.y, s_calib.ay_q16, s_calib.by_q16,
                                        (int16_t)(UI_VER_RES - 1));
        } else {
            s_last_x = pt.x;
            s_last_y = pt.y;
        }
        if (pt.pressed && !s_logged_press) {
            LOG_INFO("touch down @ %d,%d raw=%d,%d", (int)s_last_x, (int)s_last_y,
                     (int)s_raw_x, (int)s_raw_y);
            s_logged_press = true;
        }
        if (!pt.pressed) {
            s_logged_press = false;
        }
        /* Blank: first press only lights screen (no button hit-through). */
        if (s_display_blank && pt.pressed) {
            ui_display_blank_set(false);
            data->point.x = s_last_x;
            data->point.y = s_last_y;
            data->state = LV_INDEV_STATE_RELEASED;
            s_last_pressed = false;
            return;
        }
        if (pt.pressed && !was_pressed && !s_calib_running) {
            ui_display_note_activity();
        }
        if (s_calib_running) {
            if (pt.pressed && !was_pressed) {
                ui_calib_on_press_edge();
            } else if (!pt.pressed && was_pressed) {
                ui_calib_on_release_edge();
            } else if (pt.pressed && (s_calib_st == UI_CALIB_ST_HOLD)) {
                ui_calib_add_sample(s_raw_x, s_raw_y);
            }
        }
    } else if (st == IT7259_ERROR_NO_POINT) {
        if (s_last_pressed && s_calib_running) {
            s_last_pressed = false;
            s_logged_press = false;
            ui_calib_on_release_edge();
        } else {
            s_last_pressed = false;
            s_logged_press = false;
        }
    }

    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = s_last_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static status_t ui_touch_init(void)
{
    I2cDeviceConfig_t cfg = {0};
    it7259_config_t tcfg = {0};

    if (I2cProbe((s32_t)BOARD_DESKTOP_PET_TOUCH_I2C_PORT,
                 (u16_t)BOARD_DESKTOP_PET_TOUCH_I2C_ADDR) != TRUE) {
        LOG_WARN("IT7259 probe fail @0x%02X", (unsigned)BOARD_DESKTOP_PET_TOUCH_I2C_ADDR);
        return STATUS_FAIL;
    }

    cfg.port = (s32_t)BOARD_DESKTOP_PET_TOUCH_I2C_PORT;
    cfg.deviceAddress7bit = (u16_t)BOARD_DESKTOP_PET_TOUCH_I2C_ADDR;
    cfg.clockSpeedHz = 0U;
    cfg.transactionTimeoutMs = 0U;
    if (I2cRegisterDevice(&cfg) != TRUE) {
        LOG_WARN("IT7259 I2cRegisterDevice failed");
        return STATUS_FAIL;
    }

    tcfg.write_read = ui_touch_i2c_write_read;
    tcfg.address = (uint8_t)BOARD_DESKTOP_PET_TOUCH_I2C_ADDR;
    tcfg.panel_w = UI_HOR_RES;
    tcfg.panel_h = UI_VER_RES;
    tcfg.disp_w = UI_HOR_RES;
    tcfg.disp_h = UI_VER_RES;
    tcfg.invert_x = false;
    tcfg.invert_y = false;

    if (it7259_init(&s_touch, &tcfg) != IT7259_OK) {
        LOG_WARN("it7259_init failed");
        return STATUS_FAIL;
    }

    s_touch_ok = true;
    nvs_touch_calib_default(&s_calib);
    s_calib_apply = false;
    if (nvs_touch_calib_get(&s_calib) && (s_calib.valid != 0U)) {
        s_calib_apply = true;
        LOG_INFO("IT7259 touch ready (poll) calib ax=%d bx=%d ay=%d by=%d", (int)s_calib.ax_q16,
                 (int)s_calib.bx_q16, (int)s_calib.ay_q16, (int)s_calib.by_q16);
    } else {
        nvs_touch_calib_default(&s_calib);
        LOG_INFO("IT7259 touch ready (poll, no calib)");
    }
    return STATUS_OK;
}

static void ui_calib_overlay_destroy(void)
{
    if (s_calib_timer != NULL) {
        lv_timer_delete(s_calib_timer);
        s_calib_timer = NULL;
    }
    if (s_calib_layer != NULL) {
        lv_obj_delete(s_calib_layer);
        s_calib_layer = NULL;
        s_calib_mark = NULL;
        s_calib_lbl = NULL;
        (void)memset(s_calib_orient_lbl, 0, sizeof(s_calib_orient_lbl));
    }
    s_calib_st = UI_CALIB_ST_IDLE;
    s_calib_running = false;
    s_calib_step = 0U;
    s_calib_apply = (s_calib.valid != 0U);
}

static void ui_calib_orient_clear(void)
{
    uint8_t i;

    for (i = 0U; i < 5U; i++) {
        if (s_calib_orient_lbl[i] != NULL) {
            lv_obj_delete(s_calib_orient_lbl[i]);
            s_calib_orient_lbl[i] = NULL;
        }
    }
}

static lv_obj_t *ui_calib_make_label(lv_obj_t *parent, const char *txt, lv_align_t align, int32_t x_ofs,
                                     int32_t y_ofs, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
#endif
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, align, x_ofs, y_ofs);
    return lbl;
}

/** 正方向确认：Needs=上、Dock=下（与主界面一致）；点中央开始 12 点校准 */
static void ui_calib_show_orient(void)
{
    ui_calib_orient_clear();
    if (s_calib_mark != NULL) {
        lv_obj_add_flag(s_calib_mark, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_calib_lbl != NULL) {
        lv_obj_add_flag(s_calib_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    s_calib_orient_lbl[0] =
        ui_calib_make_label(s_calib_layer, "NEEDS\n(up)", LV_ALIGN_TOP_MID, 0, 10, 0x7CFF9A);
    s_calib_orient_lbl[1] =
        ui_calib_make_label(s_calib_layer, "DOCK\n(down)", LV_ALIGN_BOTTOM_MID, 0, -10, 0xFFAA66);
    s_calib_orient_lbl[2] = ui_calib_make_label(s_calib_layer, "L", LV_ALIGN_LEFT_MID, 14, 0, 0xA0E0FF);
    s_calib_orient_lbl[3] = ui_calib_make_label(s_calib_layer, "R", LV_ALIGN_RIGHT_MID, -14, 0, 0xA0E0FF);
    s_calib_orient_lbl[4] = ui_calib_make_label(s_calib_layer, "Align DOCK at bottom\nTap center",
                                                LV_ALIGN_CENTER, 0, 0, 0xE8EEF7);
    s_calib_st = UI_CALIB_ST_ORIENT;
    LOG_INFO("touch calib orient: Needs=up Dock=down (LVGL Y+ toward Dock)");
}

static void ui_calib_enter_points(void)
{
    ui_calib_orient_clear();
    if (s_calib_lbl != NULL) {
        lv_obj_remove_flag(s_calib_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_calib_mark != NULL) {
        lv_obj_remove_flag(s_calib_mark, LV_OBJ_FLAG_HIDDEN);
    }
    s_calib_idx = 0U;
    s_calib_round = 0U;
    s_calib_st = UI_CALIB_ST_WAIT_UP;
    ui_calib_set_mark(0U);
    LOG_INFO("touch calib points start (Needs->Right->Dock->Left) x3");
}

static bool ui_calib_center_ok(int16_t rx, int16_t ry)
{
    int16_t dx = (int16_t)(rx - (int16_t)(UI_HOR_RES / 2));
    int16_t dy = (int16_t)(ry - (int16_t)(UI_VER_RES / 2));

    if (dx < 0) {
        dx = (int16_t)(-dx);
    }
    if (dy < 0) {
        dy = (int16_t)(-dy);
    }
    return (dx <= 50) && (dy <= 50);
}

static void ui_calib_set_mark(uint8_t idx)
{
    char buf[28];
    uint8_t tap;
    uint8_t total;

    if (idx >= UI_CALIB_POINTS) {
        return;
    }
    s_calib_step = (uint8_t)(s_calib_round * UI_CALIB_POINTS + idx);
    tap = (uint8_t)(s_calib_step + 1U);
    total = (uint8_t)(UI_CALIB_ROUNDS * UI_CALIB_POINTS);
    if (s_calib_mark != NULL) {
        lv_obj_set_pos(s_calib_mark, (int32_t)s_calib_tx[idx] - (UI_CALIB_MARK / 2),
                       (int32_t)s_calib_ty[idx] - (UI_CALIB_MARK / 2));
        lv_obj_remove_flag(s_calib_mark, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_calib_lbl != NULL) {
        (void)snprintf(buf, sizeof(buf), "%s %u/%u", s_calib_dir[idx], (unsigned)tap,
                       (unsigned)total);
        lv_label_set_text(s_calib_lbl, buf);
        lv_obj_set_style_text_color(s_calib_lbl, lv_color_hex(0xE8EEF7), 0);
    }
}

static void ui_calib_add_sample(int16_t x, int16_t y)
{
    if (s_calib_nsamp >= UI_CALIB_MAX_SAMPLES) {
        return;
    }
    s_calib_sum_x += x;
    s_calib_sum_y += y;
    s_calib_nsamp++;
}

/** 校准中未映射：必须点在该边附近（防把 Top 误当成 Right） */
static bool ui_calib_side_ok(uint8_t idx, int16_t rx, int16_t ry)
{
    int16_t dx = (int16_t)(rx - (int16_t)(UI_HOR_RES / 2));
    int16_t dy = (int16_t)(ry - (int16_t)(UI_VER_RES / 2));

    if (dx < 0) {
        dx = (int16_t)(-dx);
    }
    if (dy < 0) {
        dy = (int16_t)(-dy);
    }

    switch (idx) {
    case UI_CALIB_IDX_TOP:
        return (ry <= 70) && (dx <= 55);
    case UI_CALIB_IDX_RIGHT:
        return (rx >= 180) && (dy <= 45);
    case UI_CALIB_IDX_BOT:
        return (ry >= 170) && (dx <= 55);
    case UI_CALIB_IDX_LEFT:
        return (rx <= 80) && (dy <= 45);
    default:
        return false;
    }
}

static int16_t ui_calib_median3(int16_t a, int16_t b, int16_t c)
{
    if (a > b) {
        int16_t t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        int16_t t = b;
        b = c;
        c = t;
    }
    if (a > b) {
        int16_t t = a;
        a = b;
        b = t;
    }
    (void)a;
    (void)c;
    return b;
}

static void ui_calib_average_rounds(void)
{
    uint8_t i;

    for (i = 0U; i < UI_CALIB_POINTS; i++) {
        s_calib_raw_x[i] = ui_calib_median3(s_calib_samp_x[i][0], s_calib_samp_x[i][1],
                                            s_calib_samp_x[i][2]);
        s_calib_raw_y[i] = ui_calib_median3(s_calib_samp_y[i][0], s_calib_samp_y[i][1],
                                            s_calib_samp_y[i][2]);
        LOG_INFO("touch calib avg %s raw=%d,%d", s_calib_dir[i], (int)s_calib_raw_x[i],
                 (int)s_calib_raw_y[i]);
    }
}

/** 左右定 X 缩放+偏移，上下定 Y（匹配圆屏极限 raw 与逻辑边不一致） */
static bool ui_calib_fit_scale_offset(int32_t *ax_q16, int32_t *bx_q16, int32_t *ay_q16,
                                      int32_t *by_q16)
{
    int32_t dx_raw;
    int32_t dy_raw;
    int32_t dx_tgt;
    int32_t dy_tgt;
    int64_t ax;
    int64_t ay;
    int64_t bx;
    int64_t by;

    dx_raw = (int32_t)s_calib_raw_x[UI_CALIB_IDX_RIGHT] - (int32_t)s_calib_raw_x[UI_CALIB_IDX_LEFT];
    dy_raw = (int32_t)s_calib_raw_y[UI_CALIB_IDX_BOT] - (int32_t)s_calib_raw_y[UI_CALIB_IDX_TOP];
    if (dx_raw < 0) {
        dx_raw = -dx_raw;
    }
    if (dy_raw < 0) {
        dy_raw = -dy_raw;
    }
    if ((dx_raw < UI_CALIB_MIN_SPAN) || (dy_raw < UI_CALIB_MIN_SPAN)) {
        LOG_WARN("touch calib span too small dx=%d dy=%d", (int)dx_raw, (int)dy_raw);
        return false;
    }

    dx_raw = (int32_t)s_calib_raw_x[UI_CALIB_IDX_RIGHT] - (int32_t)s_calib_raw_x[UI_CALIB_IDX_LEFT];
    dy_raw = (int32_t)s_calib_raw_y[UI_CALIB_IDX_BOT] - (int32_t)s_calib_raw_y[UI_CALIB_IDX_TOP];
    dx_tgt = (int32_t)s_calib_tx[UI_CALIB_IDX_RIGHT] - (int32_t)s_calib_tx[UI_CALIB_IDX_LEFT];
    dy_tgt = (int32_t)s_calib_ty[UI_CALIB_IDX_BOT] - (int32_t)s_calib_ty[UI_CALIB_IDX_TOP];

    ax = (((int64_t)dx_tgt) << 16) / (int64_t)dx_raw;
    ay = (((int64_t)dy_tgt) << 16) / (int64_t)dy_raw;
    bx = (((int64_t)s_calib_tx[UI_CALIB_IDX_LEFT]) << 16) - ax * (int64_t)s_calib_raw_x[UI_CALIB_IDX_LEFT];
    by = (((int64_t)s_calib_ty[UI_CALIB_IDX_TOP]) << 16) - ay * (int64_t)s_calib_raw_y[UI_CALIB_IDX_TOP];

    if ((ax < NVS_TOUCH_CALIB_SCALE_MIN_Q16) || (ax > NVS_TOUCH_CALIB_SCALE_MAX_Q16) ||
        (ay < NVS_TOUCH_CALIB_SCALE_MIN_Q16) || (ay > NVS_TOUCH_CALIB_SCALE_MAX_Q16)) {
        LOG_WARN("touch calib scale out of range ax=%d ay=%d (need ~0.70..1.45)", (int)ax, (int)ay);
        return false;
    }

    *ax_q16 = (int32_t)ax;
    *ay_q16 = (int32_t)ay;
    *bx_q16 = (int32_t)bx;
    *by_q16 = (int32_t)by;
    LOG_INFO("touch calib fit ax=%d bx=%d ay=%d by=%d (span x=%d y=%d)", (int)*ax_q16, (int)*bx_q16,
             (int)*ay_q16, (int)*by_q16, (int)dx_raw, (int)dy_raw);
    return true;
}

static bool ui_calib_residual_ok(int32_t ax_q16, int32_t bx_q16, int32_t ay_q16, int32_t by_q16)
{
    uint8_t i;

    for (i = 0U; i < UI_CALIB_POINTS; i++) {
        int16_t mx = ui_calib_map_axis(s_calib_raw_x[i], ax_q16, bx_q16, (int16_t)(UI_HOR_RES - 1));
        int16_t my = ui_calib_map_axis(s_calib_raw_y[i], ay_q16, by_q16, (int16_t)(UI_VER_RES - 1));
        int16_t ex = (int16_t)(mx - s_calib_tx[i]);
        int16_t ey = (int16_t)(my - s_calib_ty[i]);
        int16_t err_main;

        if (ex < 0) {
            ex = (int16_t)(-ex);
        }
        if (ey < 0) {
            ey = (int16_t)(-ey);
        }
        if ((i == UI_CALIB_IDX_TOP) || (i == UI_CALIB_IDX_BOT)) {
            err_main = ey;
        } else {
            err_main = ex;
        }
        if (err_main > UI_CALIB_FIT_MAX_RESID) {
            LOG_WARN("touch calib resid %s main=%d (ex=%d ey=%d)", s_calib_dir[i], (int)err_main,
                     (int)ex, (int)ey);
            return false;
        }
    }
    return true;
}

static bool ui_calib_commit(void)
{
    nvs_touch_calib_t cfg;
    int32_t ax_q16;
    int32_t bx_q16;
    int32_t ay_q16;
    int32_t by_q16;

    ui_calib_average_rounds();
    nvs_touch_calib_default(&cfg);
    if (!ui_calib_fit_scale_offset(&ax_q16, &bx_q16, &ay_q16, &by_q16)) {
        LOG_WARN("touch calib fit failed");
        return false;
    }
    if (!ui_calib_residual_ok(ax_q16, bx_q16, ay_q16, by_q16)) {
        LOG_WARN("touch calib residual too large, discard");
        return false;
    }
    cfg.ax_q16 = ax_q16;
    cfg.bx_q16 = bx_q16;
    cfg.ay_q16 = ay_q16;
    cfg.by_q16 = by_q16;
    cfg.valid = 1U;
    if (!nvs_touch_calib_validate(&cfg)) {
        LOG_WARN("touch calib validate fail ax=%d bx=%d ay=%d by=%d", (int)cfg.ax_q16, (int)cfg.bx_q16,
                 (int)cfg.ay_q16, (int)cfg.by_q16);
        return false;
    }
    if (!nvs_touch_calib_set(&cfg)) {
        LOG_WARN("touch calib NVS set failed");
        return false;
    }
    s_calib = cfg;
    s_calib_apply = true;
    LOG_INFO("touch calib saved ax=%d bx=%d ay=%d by=%d", (int)cfg.ax_q16, (int)cfg.bx_q16,
             (int)cfg.ay_q16, (int)cfg.by_q16);
    return true;
}

static void ui_calib_enter_end(bool ok)
{
    s_calib_st = UI_CALIB_ST_END;
    s_calib_end_ms = (int32_t)UI_CALIB_END_MS;
    s_calib_evt = UI_CALIB_EVT_NONE;
    if (!ok) {
        s_calib_apply = (s_calib.valid != 0U);
    }
    if (s_calib_mark != NULL) {
        lv_obj_add_flag(s_calib_mark, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_calib_lbl != NULL) {
        lv_label_set_text(s_calib_lbl, ok ? "Saved" : "Fail");
        lv_obj_set_style_text_color(s_calib_lbl,
                                    ok ? lv_color_hex(0x7CFF9A) : lv_color_hex(0xFF6666), 0);
    }
}

static void ui_calib_accept_point(void)
{
    int16_t rx;
    int16_t ry;

    rx = (int16_t)(s_calib_sum_x / (int32_t)s_calib_nsamp);
    ry = (int16_t)(s_calib_sum_y / (int32_t)s_calib_nsamp);

    if (!ui_calib_side_ok(s_calib_idx, rx, ry)) {
        LOG_INFO("touch calib %s wrong side raw=%d,%d", s_calib_dir[s_calib_idx], (int)rx, (int)ry);
        if (s_calib_lbl != NULL) {
            lv_label_set_text(s_calib_lbl, "Wrong side");
            lv_obj_set_style_text_color(s_calib_lbl, lv_color_hex(0xFFAA66), 0);
        }
        s_calib_st = UI_CALIB_ST_WAIT_DOWN;
        return;
    }

    s_calib_samp_x[s_calib_idx][s_calib_round] = rx;
    s_calib_samp_y[s_calib_idx][s_calib_round] = ry;
    LOG_INFO("touch calib r%u %s raw=%d,%d tgt=%d,%d", (unsigned)(s_calib_round + 1U),
             s_calib_dir[s_calib_idx], (int)rx, (int)ry, (int)s_calib_tx[s_calib_idx],
             (int)s_calib_ty[s_calib_idx]);

    s_calib_idx++;
    if (s_calib_idx >= UI_CALIB_POINTS) {
        s_calib_idx = 0U;
        s_calib_round++;
        if (s_calib_round >= UI_CALIB_ROUNDS) {
            ui_calib_enter_end(ui_calib_commit());
            return;
        }
        LOG_INFO("touch calib round %u/%u", (unsigned)(s_calib_round + 1U),
                 (unsigned)UI_CALIB_ROUNDS);
    }
    s_calib_st = UI_CALIB_ST_WAIT_UP;
}

/* indev 内只改状态/采样，不碰 LVGL / NVS */
static void ui_calib_on_press_edge(void)
{
    if ((s_calib_st != UI_CALIB_ST_WAIT_DOWN) && (s_calib_st != UI_CALIB_ST_ORIENT)) {
        return;
    }
    s_calib_orient_tap = (s_calib_st == UI_CALIB_ST_ORIENT);
    s_calib_st = UI_CALIB_ST_HOLD;
    s_calib_nsamp = 0U;
    s_calib_sum_x = 0;
    s_calib_sum_y = 0;
    ui_calib_add_sample(s_raw_x, s_raw_y);
}

static void ui_calib_on_release_edge(void)
{
    if ((s_calib_st == UI_CALIB_ST_HOLD) || (s_calib_st == UI_CALIB_ST_WAIT_UP)) {
        s_calib_evt = UI_CALIB_EVT_RELEASE;
    }
}

static void ui_calib_timer_cb(lv_timer_t *timer)
{
    uint8_t evt;

    (void)timer;
    evt = s_calib_evt;
    s_calib_evt = UI_CALIB_EVT_NONE;

    if (evt == UI_CALIB_EVT_RELEASE) {
        if (s_calib_st == UI_CALIB_ST_WAIT_UP) {
            s_calib_st = UI_CALIB_ST_WAIT_DOWN;
            ui_calib_set_mark(s_calib_idx);
        } else if (s_calib_st == UI_CALIB_ST_HOLD) {
            int16_t rx;
            int16_t ry;

            if (s_calib_nsamp < UI_CALIB_MIN_SAMPLES) {
                if (s_calib_orient_tap) {
                    s_calib_st = UI_CALIB_ST_ORIENT;
                } else {
                    s_calib_st = UI_CALIB_ST_WAIT_DOWN;
                }
                return;
            }
            rx = (int16_t)(s_calib_sum_x / (int32_t)s_calib_nsamp);
            ry = (int16_t)(s_calib_sum_y / (int32_t)s_calib_nsamp);
            if (s_calib_orient_tap) {
                s_calib_orient_tap = false;
                if (ui_calib_center_ok(rx, ry)) {
                    LOG_INFO("touch calib orient confirmed raw=%d,%d", (int)rx, (int)ry);
                    ui_calib_enter_points();
                } else {
                    LOG_INFO("touch calib orient need center raw=%d,%d", (int)rx, (int)ry);
                    if (s_calib_orient_lbl[4] != NULL) {
                        lv_label_set_text(s_calib_orient_lbl[4], "Tap CENTER\nto start");
                        lv_obj_set_style_text_color(s_calib_orient_lbl[4], lv_color_hex(0xFFAA66),
                                                    0);
                    }
                    s_calib_st = UI_CALIB_ST_ORIENT;
                }
            } else {
                ui_calib_accept_point();
            }
        }
    }

    if (s_calib_st == UI_CALIB_ST_WAIT_UP) {
        if (!s_last_pressed) {
            s_calib_st = UI_CALIB_ST_WAIT_DOWN;
            ui_calib_set_mark(s_calib_idx);
        }
        return;
    }

    if (s_calib_st == UI_CALIB_ST_END) {
        s_calib_end_ms -= (int32_t)UI_CALIB_TIMER_MS;
        if (s_calib_end_ms <= 0) {
            ui_calib_overlay_destroy();
        }
    }
}

static void ui_calib_begin(void)
{
    lv_obj_t *dot;

    if (!s_touch_ok) {
        return;
    }
    ui_calib_overlay_destroy();

    s_calib_apply = false;
    s_calib_idx = 0U;
    s_calib_round = 0U;
    s_calib_step = 0U;
    s_calib_evt = UI_CALIB_EVT_NONE;
    s_calib_orient_tap = false;
    s_calib_running = true;
    (void)memset(s_calib_samp_x, 0, sizeof(s_calib_samp_x));
    (void)memset(s_calib_samp_y, 0, sizeof(s_calib_samp_y));
    (void)memset(s_calib_orient_lbl, 0, sizeof(s_calib_orient_lbl));

    s_calib_layer = lv_obj_create(lv_layer_top());
    if (s_calib_layer == NULL) {
        s_calib_running = false;
        s_calib_st = UI_CALIB_ST_IDLE;
        s_calib_apply = (s_calib.valid != 0U);
        LOG_WARN("touch calib overlay alloc failed");
        return;
    }
    lv_obj_remove_style_all(s_calib_layer);
    lv_obj_set_size(s_calib_layer, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_style_bg_color(s_calib_layer, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(s_calib_layer, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_calib_layer, 0, 0);
    lv_obj_add_flag(s_calib_layer, LV_OBJ_FLAG_CLICKABLE);

    s_calib_lbl = lv_label_create(s_calib_layer);
    lv_label_set_text(s_calib_lbl, "Needs 1/12");
    lv_obj_set_style_text_color(s_calib_lbl, lv_color_hex(0xE8EEF7), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_calib_lbl, &lv_font_montserrat_14, 0);
#endif
    lv_obj_align(s_calib_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_calib_lbl, LV_OBJ_FLAG_HIDDEN);

    s_calib_mark = lv_obj_create(s_calib_layer);
    lv_obj_remove_style_all(s_calib_mark);
    lv_obj_set_size(s_calib_mark, UI_CALIB_MARK, UI_CALIB_MARK);
    lv_obj_set_style_radius(s_calib_mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_calib_mark, 2, 0);
    lv_obj_set_style_border_color(s_calib_mark, lv_color_hex(0xFF6666), 0);
    lv_obj_set_style_bg_opa(s_calib_mark, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_calib_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_calib_mark, LV_OBJ_FLAG_HIDDEN);

    dot = lv_obj_create(s_calib_mark);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, UI_CALIB_DOT, UI_CALIB_DOT);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF6666), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_center(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    ui_calib_show_orient();
    s_calib_timer = lv_timer_create(ui_calib_timer_cb, UI_CALIB_TIMER_MS, NULL);
    if (s_calib_timer == NULL) {
        LOG_WARN("touch calib timer failed");
        ui_calib_overlay_destroy();
        return;
    }
    LOG_INFO("touch calib started (orient first)");
}

static void ui_calib_reset_runtime(void)
{
    ui_calib_overlay_destroy();
    (void)nvs_touch_calib_delete();
    nvs_touch_calib_default(&s_calib);
    s_calib_apply = false;
    LOG_INFO("touch calib cleared");
}

static void ui_calib_take_req(void)
{
    int req = s_calib_req;

    if (req == UI_CALIB_REQ_NONE) {
        return;
    }
    s_calib_req = UI_CALIB_REQ_NONE;
    if (req == UI_CALIB_REQ_START) {
        ui_calib_begin();
    } else if (req == UI_CALIB_REQ_CANCEL) {
        if (s_calib_running) {
            ui_calib_overlay_destroy();
            LOG_INFO("touch calib cancelled");
        }
    } else if (req == UI_CALIB_REQ_RESET) {
        ui_calib_reset_runtime();
    }
}
#endif /* DESKTOP_PET_ENABLE_TOUCH */

#if DESKTOP_PET_ENABLE_DEBUG_UI
static void ui_rec_status_set(const char *text, bool recording)
{
    if (s_lbl_rec != NULL) {
        lv_label_set_text(s_lbl_rec, text);
        lv_obj_set_style_text_color(s_lbl_rec,
                                    recording ? lv_color_hex(0xFF6666) : lv_color_hex(0xA0E0FF), 0);
    }
    if (s_btn_rec_lbl != NULL) {
        lv_label_set_text(s_btn_rec_lbl, recording ? "Stop" : "Rec");
    }
    s_play_shown = false;
}

static void ui_play_status_set(const char *text, bool playing)
{
    if (s_lbl_rec != NULL) {
        lv_label_set_text(s_lbl_rec, text);
        lv_obj_set_style_text_color(s_lbl_rec,
                                    playing ? lv_color_hex(0x7CFF9A) : lv_color_hex(0xA0E0FF), 0);
    }
    s_play_shown = playing;
}

static void ui_rec_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!desktop_pet_audio_is_ready()) {
        ui_rec_status_set("Rec: N/A", false);
        return;
    }
    if (desktop_pet_audio_is_recording() || (s_rec_req == 1)) {
        s_rec_req = 2;
        ui_rec_status_set("Stopping...", true);
    } else {
        s_rec_req = 1;
        ui_rec_status_set("Starting...", true);
    }
}

static void ui_play_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!desktop_pet_audio_is_ready()) {
        ui_play_status_set("Play: N/A", false);
        return;
    }
    if (desktop_pet_audio_is_recording()) {
        ui_rec_status_set("Rec first", true);
        return;
    }
    if (desktop_pet_audio_pcm_bytes() == 0U) {
        ui_play_status_set("No rec", false);
        return;
    }
    s_play_req = 1;
    ui_play_status_set("Starting...", true);
}

static void ui_pause_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!desktop_pet_audio_is_playing()) {
        ui_play_status_set("Idle", false);
        return;
    }
    s_play_req = 2;
    ui_play_status_set("Pausing...", true);
}

static void ui_talk_status_set(bool listening)
{
    if (s_btn_talk_lbl != NULL) {
        lv_label_set_text(s_btn_talk_lbl, listening ? "Stop" : "Talk");
    }
}

static void ui_conn_status_set(desktop_pet_agent_state_t st)
{
    const char *txt = "Conn";

    if (s_btn_conn_lbl == NULL) {
        return;
    }
    switch (st) {
    case DESKTOP_PET_AGENT_STATE_CONNECTING:
        txt = "...";
        break;
    case DESKTOP_PET_AGENT_STATE_OPEN:
        txt = "Disc";
        break;
    case DESKTOP_PET_AGENT_STATE_LISTENING:
        txt = "Disc";
        break;
    case DESKTOP_PET_AGENT_STATE_SPEAKING:
        txt = "Disc";
        break;
    case DESKTOP_PET_AGENT_STATE_ERROR:
        txt = "Retry";
        break;
    default:
        txt = "Conn";
        break;
    }
    lv_label_set_text(s_btn_conn_lbl, txt);
}

static void ui_rec_poll_timer_cb(lv_timer_t *timer)
{
    char buf[40];
    desktop_pet_agent_state_t agent_st;

    (void)timer;

    agent_st = desktop_pet_agent_get_state();
    ui_talk_status_set(desktop_pet_agent_is_listen_active());
    ui_conn_status_set(agent_st);

    if (s_rec_req == 1) {
        s_rec_req = 0;
        if (agent_st == DESKTOP_PET_AGENT_STATE_LISTENING || agent_st == DESKTOP_PET_AGENT_STATE_SPEAKING ||
            desktop_pet_audio_is_playouting() || desktop_pet_audio_is_streaming()) {
            ui_rec_status_set("Busy", false);
            LOG_WARN("audio: Rec ignored, agent busy");
        } else if (desktop_pet_audio_record_start() != STATUS_OK) {
            ui_rec_status_set("Rec fail", false);
            LOG_ERROR("audio record_start failed");
        } else {
            ui_rec_status_set("Recording...", true);
        }
    } else if (s_rec_req == 2) {
        char path[64];

        s_rec_req = 0;
        (void)desktop_pet_audio_record_stop();
        if (desktop_pet_audio_save_to_sd(path, sizeof(path)) == STATUS_OK) {
            ui_rec_status_set("Saved", false);
            LOG_INFO("audio saved: %s", path);
        } else {
            (void)snprintf(buf, sizeof(buf), "Idle %uB", (unsigned)desktop_pet_audio_pcm_bytes());
            ui_rec_status_set(buf, false);
        }
    }

    if (s_play_req == 1) {
        s_play_req = 0;
        if (agent_st == DESKTOP_PET_AGENT_STATE_LISTENING || agent_st == DESKTOP_PET_AGENT_STATE_SPEAKING ||
            desktop_pet_audio_is_playouting() || desktop_pet_audio_is_streaming()) {
            ui_play_status_set("Busy", false);
            LOG_WARN("audio: Play ignored, agent busy");
        } else if (desktop_pet_audio_play_start() != STATUS_OK) {
            ui_play_status_set("Play fail", false);
        } else {
            ui_play_status_set("Playing", true);
        }
    } else if (s_play_req == 2) {
        s_play_req = 0;
        (void)desktop_pet_audio_play_pause();
        ui_play_status_set("Paused", true);
    }

    if (!desktop_pet_audio_is_ready()) {
        return;
    }
    if (desktop_pet_audio_is_recording()) {
        (void)snprintf(buf, sizeof(buf), "REC %uB", (unsigned)desktop_pet_audio_pcm_bytes());
        ui_rec_status_set(buf, true);
    } else if (desktop_pet_audio_is_paused()) {
        ui_play_status_set("Paused", true);
    } else if (desktop_pet_audio_is_playing()) {
        ui_play_status_set("Playing", true);
    } else if (s_play_shown) {
        ui_play_status_set("Idle", false);
        s_play_shown = false;
    }
}

static lv_obj_t *ui_angle_label_create(lv_obj_t *parent, const char *text, int32_t y_ofs)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_label_set_text(lbl, text);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lbl, 0, 0);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xA0E0FF), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
#endif
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y_ofs);
    return lbl;
}

static void ui_ip_timer_cb(lv_timer_t *timer)
{
    char ip[20];
    char buf[28];

    (void)timer;
    if (s_lbl_ip == NULL) {
        return;
    }
    if (net_wifi_format_ipv4_for_display(ip, sizeof(ip))) {
        (void)snprintf(buf, sizeof(buf), "%s", ip);
    } else {
        (void)snprintf(buf, sizeof(buf), "IP: ---");
    }
    lv_label_set_text(s_lbl_ip, buf);
}

static void ui_apply_debug_visible(void)
{
    if (s_debug == NULL) {
        return;
    }
    if (s_debug_on) {
        lv_obj_remove_flag(s_debug, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_debug, LV_OBJ_FLAG_HIDDEN);
    }
    LOG_INFO("debug overlay %s", s_debug_on ? "on" : "off");
}
#endif /* DESKTOP_PET_ENABLE_DEBUG_UI */

static void ui_intent_hook(const pet_intent_t *in)
{
    if (in == NULL) {
        return;
    }
    if (in->id == PET_INTENT_HUD) {
        pet_needs_t n;
        nvs_pet_needs_t st;

        pet_core_get_needs(&n);
        st.magic = NVS_PET_NEEDS_MAGIC;
        st.hunger = n.hunger;
        st.mood = n.mood;
        st.energy = n.energy;
        st.sleeping = n.sleeping ? 1U : 0U;
        if (!nvs_pet_needs_set(&st)) {
            LOG_WARN("pet needs NVS save failed");
        }
    } else if (in->id == PET_INTENT_LED) {
        if (in->arg0 == 0) {
            led_scene_run(LED_SCENE_ID_SUCCESS);
        } else {
            led_scene_run(LED_SCENE_ID_TRIGGER);
        }
    } else if (in->id == PET_INTENT_OPEN_CHAT) {
        /* Chat surface opens inside pet_view; keep hook for LED/SFX later. */
        (void)in;
    } else if (in->id == PET_INTENT_MOTOR) {
        /* 量产路径暂未接 TB6612；显式吞掉避免误以为已驱动。 */
        (void)in;
    } else if (in->id == PET_INTENT_SFX) {
        pet_sfx_play_for_clip((pet_clip_id_t)in->arg0);
    }
}

static void ui_chat_hook(pet_chat_act_t act)
{
    switch (act) {
    case PET_CHAT_ACT_ENTER:
        LOG_INFO("chat: enter → session");
        ui_display_note_activity();
        /* Phase CHAT: drop WakeNet before WS/capture eat internal SRAM. */
        desktop_pet_wake_enter_chat_mode();
        s_chat_agent_prev = desktop_pet_agent_get_state();
        s_chat_reconnect_cool = 0U;
        (void)desktop_pet_agent_session_open();
        break;
    case PET_CHAT_ACT_LEAVE:
        LOG_INFO("chat: leave → stop");
        ui_display_note_activity();
        s_chat_reconnect_cool = 0U;
        (void)desktop_pet_agent_session_close();
        s_chat_agent_prev = DESKTOP_PET_AGENT_STATE_IDLE;
        desktop_pet_wake_leave_chat_mode();
        break;
    case PET_CHAT_ACT_LISTEN_ON:
        LOG_INFO("chat: tap → listen");
        ui_display_note_activity();
        s_chat_reconnect_cool = 0U;
        (void)desktop_pet_agent_listen_start();
        break;
    case PET_CHAT_ACT_LISTEN_OFF:
        LOG_INFO("chat: tap → wait answer");
        ui_display_note_activity();
        s_chat_reconnect_cool = 0U;
        (void)desktop_pet_agent_listen_stop();
        break;
    default:
        break;
    }
}

static void ui_agent_ui_cb(desktop_pet_agent_ui_evt_t evt, const char *text)
{
    uint8_t kind = 0;

    if (evt == DESKTOP_PET_AGENT_UI_STATE) {
        return;
    }
    if ((text == NULL) || (text[0] == '\0')) {
        return;
    }
    if (evt == DESKTOP_PET_AGENT_UI_STT) {
        kind = 1U;
    } else if (evt == DESKTOP_PET_AGENT_UI_TTS_TEXT) {
        kind = 2U;
    } else if (evt == DESKTOP_PET_AGENT_UI_LLM) {
        kind = 3U;
    } else if (evt == DESKTOP_PET_AGENT_UI_NET) {
        kind = 4U;
    } else {
        return;
    }
    portENTER_CRITICAL(&s_chat_cap_mux);
    strncpy(s_chat_cap_pending, text, sizeof(s_chat_cap_pending) - 1U);
    s_chat_cap_pending[sizeof(s_chat_cap_pending) - 1U] = '\0';
    s_chat_cap_pending_kind = kind;
    portEXIT_CRITICAL(&s_chat_cap_mux);
}

static void ui_chat_sync(void)
{
    desktop_pet_agent_state_t st;
    char cap[UI_CHAT_CAPTION_MAX];
    uint8_t kind = 0;

    if (!pet_view_chat_is_open()) {
        return;
    }

    st = desktop_pet_agent_get_state();
    if (st != s_chat_agent_prev) {
        switch (st) {
        case DESKTOP_PET_AGENT_STATE_CONNECTING:
            pet_view_chat_set_mode(PET_CHAT_MODE_CONNECTING);
            break;
        case DESKTOP_PET_AGENT_STATE_LISTENING:
            pet_view_chat_set_mode(PET_CHAT_MODE_LISTENING);
            break;
        case DESKTOP_PET_AGENT_STATE_SPEAKING:
            pet_view_chat_set_mode(PET_CHAT_MODE_SPEAKING);
            break;
        case DESKTOP_PET_AGENT_STATE_OPEN:
            /* PTT：OPEN 只表示会话就绪/等答，不自动进听。 */
            if (!desktop_pet_agent_is_listen_active()) {
                pet_view_chat_set_mode(PET_CHAT_MODE_IDLE);
            }
            break;
        case DESKTOP_PET_AGENT_STATE_ERROR:
            pet_view_chat_set_mode(PET_CHAT_MODE_IDLE);
            /* 具体文案由 UI_NET 写入；无则兜底。 */
            break;
        case DESKTOP_PET_AGENT_STATE_IDLE:
        default:
            if (s_chat_agent_prev == DESKTOP_PET_AGENT_STATE_ERROR) {
                /* 网络失败后留在就绪，不自动重连刷 connecting。 */
                pet_view_chat_set_mode(PET_CHAT_MODE_IDLE);
            } else if (s_chat_agent_prev == DESKTOP_PET_AGENT_STATE_LISTENING ||
                       s_chat_agent_prev == DESKTOP_PET_AGENT_STATE_SPEAKING ||
                       s_chat_agent_prev == DESKTOP_PET_AGENT_STATE_OPEN ||
                       s_chat_agent_prev == DESKTOP_PET_AGENT_STATE_CONNECTING) {
                pet_view_chat_set_mode(PET_CHAT_MODE_CONNECTING);
                s_chat_reconnect_cool = 25U; /* 2.5s，避开 WS destroy 竞态 */
            }
            break;
        }
        s_chat_agent_prev = st;
    }

    if (s_chat_reconnect_cool > 0U) {
        s_chat_reconnect_cool--;
        if (s_chat_reconnect_cool == 0U && pet_view_chat_is_open()) {
            desktop_pet_agent_state_t cur = desktop_pet_agent_get_state();

            if (cur == DESKTOP_PET_AGENT_STATE_IDLE || cur == DESKTOP_PET_AGENT_STATE_ERROR) {
                LOG_INFO("chat: ws lost → reconnect session");
                (void)desktop_pet_agent_session_open();
            }
        }
    }

    portENTER_CRITICAL(&s_chat_cap_mux);
    kind = s_chat_cap_pending_kind;
    if (kind != 0U) {
        memcpy(cap, s_chat_cap_pending, sizeof(cap));
        s_chat_cap_pending_kind = 0U;
    }
    portEXIT_CRITICAL(&s_chat_cap_mux);
    if (kind != 0U) {
        pet_view_chat_set_caption(cap);
        pet_view_chat_bump_idle();
    }
}

static void ui_gesture_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    desktop_pet_wake_ui_poll();
    ui_chat_sync();
    ui_blank_poll();

    if (s_shake_cool_ms > UI_GESTURE_PERIOD_MS) {
        s_shake_cool_ms -= UI_GESTURE_PERIOD_MS;
    } else {
        s_shake_cool_ms = 0U;
    }

#if !DESKTOP_PET_ENABLE_IMU
#if DESKTOP_PET_ENABLE_DEBUG_UI
    if (s_debug_on && (s_lbl_roll != NULL)) {
        lv_label_set_text(s_lbl_roll, "R: --");
        lv_label_set_text(s_lbl_pitch, "P: --");
        lv_label_set_text(s_lbl_yaw, "Y: --");
    }
#endif
#else
    {
    qmi8658a_t *imu;
    int16_t ax, ay, az, gx, gy, gz;
    float ax_g, ay_g, az_g;
    float mag;
#if DESKTOP_PET_ENABLE_DEBUG_UI
    char buf[24];
#endif

    imu = BoardQmi8658();
    if ((imu == NULL) || !imu->initialized) {
#if DESKTOP_PET_ENABLE_DEBUG_UI
        if (s_debug_on && (s_lbl_roll != NULL)) {
            lv_label_set_text(s_lbl_roll, "R: --");
            lv_label_set_text(s_lbl_pitch, "P: --");
            lv_label_set_text(s_lbl_yaw, "Y: --");
        }
#endif
        return;
    }

    if (qmi8658a_read_raw(imu, &ax, &ay, &az, &gx, &gy, &gz) != QMI8658A_OK) {
        return;
    }

    ax_g = (float)ax / UI_ACCEL_LSB_PER_G;
    ay_g = (float)ay / UI_ACCEL_LSB_PER_G;
    az_g = (float)az / UI_ACCEL_LSB_PER_G;
    mag = sqrtf(ax_g * ax_g + ay_g * ay_g + az_g * az_g);

    if (mag > UI_SHAKE_G) {
        s_shake_hits++;
#if DESKTOP_PET_ENABLE_DEBUG_UI
        if ((s_shake_hits >= UI_SHAKE_HITS) && (s_shake_cool_ms == 0U) && !s_debug_on) {
#else
        if ((s_shake_hits >= UI_SHAKE_HITS) && (s_shake_cool_ms == 0U)) {
#endif
            s_shake_hits = 0U;
            s_shake_cool_ms = UI_SHAKE_COOLDOWN_MS;
            (void)pet_core_post(PET_EVT_IMU_SHAKE, 0);
        }
    } else {
        s_shake_hits = 0U;
    }

    if (az_g < UI_FLIP_G) {
        s_flip_hits++;
#if DESKTOP_PET_ENABLE_DEBUG_UI
        if ((s_flip_hits >= UI_FLIP_HITS) && !s_debug_on) {
#else
        if (s_flip_hits >= UI_FLIP_HITS) {
#endif
            s_flip_hits = 0U;
            (void)pet_core_post(PET_EVT_IMU_FLIP, 0);
        }
    } else {
        s_flip_hits = 0U;
    }

#if DESKTOP_PET_ENABLE_DEBUG_UI
    if (!s_debug_on || (s_lbl_roll == NULL)) {
        return;
    }

    {
        float roll = atan2f(ay_g, az_g) * UI_RAD2DEG;
        float pitch = atan2f(-ax_g, sqrtf(ay_g * ay_g + az_g * az_g)) * UI_RAD2DEG;
        float dt = (float)UI_GESTURE_PERIOD_MS / 1000.0f;

        s_yaw_deg += ((float)gz / UI_GYRO_LSB_PER_DPS) * dt;
        if (s_yaw_deg > 180.0f) {
            s_yaw_deg -= 360.0f;
        } else if (s_yaw_deg < -180.0f) {
            s_yaw_deg += 360.0f;
        }
        (void)snprintf(buf, sizeof(buf), "R:%5.1f", (double)roll);
        lv_label_set_text(s_lbl_roll, buf);
        (void)snprintf(buf, sizeof(buf), "P:%5.1f", (double)pitch);
        lv_label_set_text(s_lbl_pitch, buf);
        (void)snprintf(buf, sizeof(buf), "Y:%5.1f", (double)s_yaw_deg);
        lv_label_set_text(s_lbl_yaw, buf);
    }
#endif
    }
#endif
}

#if DESKTOP_PET_ENABLE_DEBUG_UI
static void ui_talk_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    (void)desktop_pet_agent_listen_toggle();
}

static void ui_conn_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ui_conn_status_set(DESKTOP_PET_AGENT_STATE_CONNECTING);
    (void)desktop_pet_agent_session_toggle();
}

static void ui_debug_overlay_create(lv_obj_t *parent)
{
    lv_obj_t *btn_rec;
    lv_obj_t *btn_play;
    lv_obj_t *btn_pause;
    lv_obj_t *btn_talk;
    lv_obj_t *btn_conn;
    lv_obj_t *play_lbl;
    lv_obj_t *pause_lbl;
    lv_obj_t *title;

    s_debug = lv_obj_create(parent);
    lv_obj_remove_style_all(s_debug);
    lv_obj_set_size(s_debug, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_style_bg_color(s_debug, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(s_debug, LV_OPA_COVER, 0);
    lv_obj_align(s_debug, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_debug, LV_OBJ_FLAG_HIDDEN);

    title = lv_label_create(s_debug);
    lv_label_set_text(title, "DEBUG");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFAA66), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_lbl_roll = ui_angle_label_create(s_debug, "R:  0.0", 28);
    s_lbl_pitch = ui_angle_label_create(s_debug, "P:  0.0", 46);
    s_lbl_yaw = ui_angle_label_create(s_debug, "Y:  0.0", 64);
    s_lbl_rec = ui_angle_label_create(s_debug, "Idle", 82);

    s_lbl_ip = lv_label_create(s_debug);
    lv_label_set_text(s_lbl_ip, "IP: ---");
    lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(0xA0E0FF), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_lbl_ip, &lv_font_montserrat_14, 0);
#endif
    lv_obj_align(s_lbl_ip, LV_ALIGN_BOTTOM_MID, 0, -8);
    (void)lv_timer_create(ui_ip_timer_cb, 1000, NULL);
    ui_ip_timer_cb(NULL);

    btn_rec = lv_button_create(s_debug);
    lv_obj_set_size(btn_rec, 64, 32);
    lv_obj_align(btn_rec, LV_ALIGN_CENTER, -40, -4);
    lv_obj_add_event_cb(btn_rec, ui_rec_btn_cb, LV_EVENT_PRESSED, NULL);
    s_btn_rec_lbl = lv_label_create(btn_rec);
    lv_label_set_text(s_btn_rec_lbl, "Rec");
    lv_obj_center(s_btn_rec_lbl);

    btn_play = lv_button_create(s_debug);
    lv_obj_set_size(btn_play, 64, 32);
    lv_obj_align(btn_play, LV_ALIGN_CENTER, 40, -4);
    lv_obj_add_event_cb(btn_play, ui_play_btn_cb, LV_EVENT_PRESSED, NULL);
    play_lbl = lv_label_create(btn_play);
    lv_label_set_text(play_lbl, "Play");
    lv_obj_center(play_lbl);

    btn_pause = lv_button_create(s_debug);
    lv_obj_set_size(btn_pause, 64, 32);
    lv_obj_align(btn_pause, LV_ALIGN_CENTER, -40, 36);
    lv_obj_add_event_cb(btn_pause, ui_pause_btn_cb, LV_EVENT_PRESSED, NULL);
    pause_lbl = lv_label_create(btn_pause);
    lv_label_set_text(pause_lbl, "Pause");
    lv_obj_center(pause_lbl);

    btn_talk = lv_button_create(s_debug);
    lv_obj_set_size(btn_talk, 64, 32);
    lv_obj_align(btn_talk, LV_ALIGN_CENTER, 40, 36);
    lv_obj_add_event_cb(btn_talk, ui_talk_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_talk_lbl = lv_label_create(btn_talk);
    lv_label_set_text(s_btn_talk_lbl, "Talk");
    lv_obj_center(s_btn_talk_lbl);

    btn_conn = lv_button_create(s_debug);
    lv_obj_set_size(btn_conn, 96, 32);
    lv_obj_align(btn_conn, LV_ALIGN_CENTER, 0, 76);
    lv_obj_add_event_cb(btn_conn, ui_conn_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_conn_lbl = lv_label_create(btn_conn);
    lv_label_set_text(s_btn_conn_lbl, "Conn");
    lv_obj_center(s_btn_conn_lbl);

    (void)lv_timer_create(ui_rec_poll_timer_cb, 500, NULL);
    if (!desktop_pet_audio_is_ready()) {
        ui_rec_status_set("Rec: N/A", false);
    }
}
#endif /* DESKTOP_PET_ENABLE_DEBUG_UI */

static void ui_on_pet_home(lv_obj_t *scr)
{
#if DESKTOP_PET_ENABLE_DEBUG_UI
    ui_debug_overlay_create(scr);
#else
    (void)scr;
#endif
    (void)lv_timer_create(ui_gesture_timer_cb, UI_GESTURE_PERIOD_MS, NULL);
    desktop_pet_wake_set_home_active(true);
}

static void ui_screen_pet_create(void)
{
    lv_obj_t *scr = lv_screen_active();

    {
        const char *root = sd_cfg_content_path();

        if ((root == NULL) || (root[0] == '\0')) {
            root = "/sdcard/pet";
        }
        pet_fs_set_root(root);
    }
    pet_view_set_alloc(ui_alloc_psram, ui_free_psram);
    pet_view_set_intent_hook(ui_intent_hook);
    pet_view_set_chat_hook(ui_chat_hook);
    desktop_pet_agent_set_ui_cb(ui_agent_ui_cb);
    pet_view_boot_start(scr, ui_on_pet_home);

    if (pet_res_load()) {
        pet_core_init(pet_res_needs_cfg());
        LOG_INFO("pet pack loaded root=%s", pet_fs_root());
    } else {
        pet_core_init(NULL);
        LOG_WARN("pet pack missing (%s); fallback body", pet_fs_root());
    }
    {
        nvs_pet_needs_t st;
        pet_needs_t n;

        if (nvs_pet_needs_get(&st)) {
            n.hunger = st.hunger;
            n.mood = st.mood;
            n.energy = st.energy;
            n.sleeping = (st.sleeping != 0U);
            pet_core_set_needs(&n);
            LOG_INFO("pet needs from NVS h=%u m=%u e=%u sleep=%u", (unsigned)n.hunger,
                     (unsigned)n.mood, (unsigned)n.energy, (unsigned)st.sleeping);
        } else {
            pet_core_get_needs(&n);
            st.magic = NVS_PET_NEEDS_MAGIC;
            st.hunger = n.hunger;
            st.mood = n.mood;
            st.energy = n.energy;
            st.sleeping = 0U;
            (void)nvs_pet_needs_set(&st);
        }
    }
    pet_view_boot_pack_done();
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t delay_ms;

#if DESKTOP_PET_ENABLE_TOUCH
        ui_calib_take_req();
#endif
#if DESKTOP_PET_ENABLE_DEBUG_UI
        if (s_debug_req != 0) {
            s_debug_req = 0;
            s_debug_on = !s_debug_on;
            ui_apply_debug_visible();
        }
#endif

        delay_ms = lv_timer_handler();
        if (delay_ms > 50U) {
            delay_ms = 50U;
        }
        if (delay_ms < 1U) {
            delay_ms = 1U;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void desktop_pet_ui_toggle_debug(void)
{
#if DESKTOP_PET_ENABLE_DEBUG_UI
    s_debug_req = 1;
#else
    /* Product home: GPIO0 single-click reserved (debug overlay isolated). */
#endif
}

bool desktop_pet_ui_touch_calib_start(void)
{
#if DESKTOP_PET_ENABLE_TOUCH
    if (!s_started || !s_touch_ok) {
        return false;
    }
    if (s_calib_running) {
        return true;
    }
    s_calib_req = UI_CALIB_REQ_START;
    return true;
#else
    return false;
#endif
}

bool desktop_pet_ui_touch_calib_cancel(void)
{
#if DESKTOP_PET_ENABLE_TOUCH
    if (!s_calib_running && (s_calib_req != UI_CALIB_REQ_START)) {
        return false;
    }
    s_calib_req = UI_CALIB_REQ_CANCEL;
    return true;
#else
    return false;
#endif
}

bool desktop_pet_ui_touch_calib_reset(void)
{
#if DESKTOP_PET_ENABLE_TOUCH
    if (!s_started) {
        return nvs_touch_calib_delete();
    }
    s_calib_req = UI_CALIB_REQ_RESET;
    return true;
#else
    return nvs_touch_calib_delete();
#endif
}

bool desktop_pet_ui_touch_calib_is_running(void)
{
#if DESKTOP_PET_ENABLE_TOUCH
    return s_calib_running || (s_calib_req == UI_CALIB_REQ_START);
#else
    return false;
#endif
}

void desktop_pet_ui_touch_calib_status(bool *running, uint8_t *step, uint8_t *steps, bool *saved)
{
    if (running != NULL) {
        *running = desktop_pet_ui_touch_calib_is_running();
    }
    if (step != NULL) {
#if DESKTOP_PET_ENABLE_TOUCH
        *step = s_calib_step;
#else
        *step = 0U;
#endif
    }
    if (steps != NULL) {
        *steps = (uint8_t)(UI_CALIB_ROUNDS * UI_CALIB_POINTS);
    }
    if (saved != NULL) {
        nvs_touch_calib_t cfg;
        *saved = nvs_touch_calib_get(&cfg) && (cfg.valid != 0U);
    }
}

status_t desktop_pet_ui_start(void)
{
    gc9a01_t *lcd;
    size_t buf_bytes;
    esp_timer_create_args_t tick_args = {
        .callback = ui_tick_cb,
        .name = "lv_tick",
    };

    if (s_started) {
        return STATUS_OK;
    }

#if !DESKTOP_PET_ENABLE_LCD
    LOG_WARN("desktop_pet_ui: LCD disabled");
    return STATUS_FAIL;
#else
    lcd = BoardGc9a01();
    if ((lcd == NULL) || !gc9a01_is_initialized(lcd)) {
        LOG_ERROR("desktop_pet_ui: GC9A01 not ready");
        return STATUS_FAIL;
    }

    lv_init();

    buf_bytes = (size_t)UI_HOR_RES * (size_t)UI_BUF_LINES * 2U;
    s_buf1 = (uint8_t *)ui_alloc_buf(buf_bytes);
    s_buf2 = (uint8_t *)ui_alloc_buf(buf_bytes);
    if ((s_buf1 == NULL) || (s_buf2 == NULL)) {
        LOG_ERROR("desktop_pet_ui: draw buffer alloc failed (%u bytes each)", (unsigned)buf_bytes);
        heap_caps_free(s_buf1);
        heap_caps_free(s_buf2);
        s_buf1 = NULL;
        s_buf2 = NULL;
        return STATUS_FAIL;
    }

    s_disp = lv_display_create(UI_HOR_RES, UI_VER_RES);
    if (s_disp == NULL) {
        LOG_ERROR("desktop_pet_ui: lv_display_create failed");
        heap_caps_free(s_buf1);
        heap_caps_free(s_buf2);
        s_buf1 = NULL;
        s_buf2 = NULL;
        return STATUS_FAIL;
    }

    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, ui_flush_cb);

#if DESKTOP_PET_ENABLE_TOUCH
    if (ui_touch_init() == STATUS_OK) {
        s_indev = lv_indev_create();
        if (s_indev != NULL) {
            lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(s_indev, ui_touch_read_cb);
            lv_indev_set_display(s_indev, s_disp);
        } else {
            LOG_WARN("lv_indev_create failed");
        }
    } else {
        LOG_WARN("touch disabled (IT7259 not ready); UI display-only");
    }
#else
    (void)s_indev;
#endif

    if (esp_timer_create(&tick_args, &s_tick_timer) != ESP_OK) {
        LOG_ERROR("desktop_pet_ui: tick timer create failed");
        s_tick_timer = NULL;
        return STATUS_FAIL;
    }
    if (esp_timer_start_periodic(s_tick_timer, UI_TICK_PERIOD_MS * 1000ULL) != ESP_OK) {
        LOG_ERROR("desktop_pet_ui: tick timer start failed");
        (void)esp_timer_delete(s_tick_timer);
        s_tick_timer = NULL;
        return STATUS_FAIL;
    }

    /* audio 在 start.c 独立初始化，避免 LCD 失败时会话无麦。 */

    ui_screen_pet_create();

    if (xTaskCreate(ui_task, "pet_ui", UI_TASK_STACK_WORDS, NULL, UI_TASK_PRIORITY, NULL) != pdPASS) {
        LOG_ERROR("desktop_pet_ui: task create failed");
        if (s_tick_timer != NULL) {
            (void)esp_timer_stop(s_tick_timer);
            (void)esp_timer_delete(s_tick_timer);
            s_tick_timer = NULL;
        }
        return STATUS_FAIL;
    }

    ui_blank_read_cfg();
    ui_display_note_activity();
    (void)gc9a01_set_backlight(lcd, true);
    s_display_blank = false;
    s_started = true;
    LOG_INFO("desktop_pet_ui started %dx%d touch=%d", UI_HOR_RES, UI_VER_RES,
#if DESKTOP_PET_ENABLE_TOUCH
             s_touch_ok ? 1 : 0
#else
             0
#endif
    );
    return STATUS_OK;
#endif
}
