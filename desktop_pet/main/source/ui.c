/**
 * @file ui.c
 * @brief LVGL port: GC9A01 flush, IT7259, pet_view, GPIO0 debug Rec/Play overlay.
 */

#include "ui.h"

#include "agent.h"
#include "audio.h"
#include "board.h"
#include "gc9a01.h"
#include "i2c.h"
#include "it7259.h"
#include "led_scene.h"
#include "log.h"
#include "net_wifi.h"
#include "pet_core.h"
#include "pet_fs.h"
#include "pet_res.h"
#include "pet_view.h"
#include "qmi8658a.h"
#include "type.h"

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

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static uint8_t *s_buf1;
static uint8_t *s_buf2;
static esp_timer_handle_t s_tick_timer;
static bool s_started;
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
static uint8_t s_shake_hits;
static uint8_t s_flip_hits;
static uint32_t s_shake_cool_ms;

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
static bool s_last_pressed;
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
        s_last_pressed = pt.pressed;
        s_last_x = pt.x;
        s_last_y = pt.y;
        if (pt.pressed && !s_logged_press) {
            LOG_INFO("touch down @ %d,%d", (int)s_last_x, (int)s_last_y);
            s_logged_press = true;
        }
        if (!pt.pressed) {
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
    LOG_INFO("IT7259 touch ready (poll)");
    return STATUS_OK;
}
#endif /* DESKTOP_PET_ENABLE_TOUCH */

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

static void ui_intent_hook(const pet_intent_t *in)
{
    if (in == NULL) {
        return;
    }
    if (in->id == PET_INTENT_LED) {
        if (in->arg0 == 0) {
            led_scene_run(LED_SCENE_ID_SUCCESS);
        } else {
            led_scene_run(LED_SCENE_ID_TRIGGER);
        }
    } else if (in->id == PET_INTENT_MOTOR || in->id == PET_INTENT_SFX) {
        /* 量产路径暂未接 TB6612 / 音效；显式吞掉避免误以为已驱动。 */
        (void)in;
    }
}

static void ui_gesture_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_shake_cool_ms > UI_GESTURE_PERIOD_MS) {
        s_shake_cool_ms -= UI_GESTURE_PERIOD_MS;
    } else {
        s_shake_cool_ms = 0U;
    }

#if !DESKTOP_PET_ENABLE_IMU
    if (s_debug_on && (s_lbl_roll != NULL)) {
        lv_label_set_text(s_lbl_roll, "R: --");
        lv_label_set_text(s_lbl_pitch, "P: --");
        lv_label_set_text(s_lbl_yaw, "Y: --");
    }
#else
    {
    qmi8658a_t *imu;
    int16_t ax, ay, az, gx, gy, gz;
    float ax_g, ay_g, az_g;
    float mag;
    char buf[24];

    imu = BoardQmi8658();
    if ((imu == NULL) || !imu->initialized) {
        if (s_debug_on && (s_lbl_roll != NULL)) {
            lv_label_set_text(s_lbl_roll, "R: --");
            lv_label_set_text(s_lbl_pitch, "P: --");
            lv_label_set_text(s_lbl_yaw, "Y: --");
        }
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
        if ((s_shake_hits >= UI_SHAKE_HITS) && (s_shake_cool_ms == 0U) && !s_debug_on) {
            s_shake_hits = 0U;
            s_shake_cool_ms = UI_SHAKE_COOLDOWN_MS;
            (void)pet_core_post(PET_EVT_IMU_SHAKE, 0);
        }
    } else {
        s_shake_hits = 0U;
    }

    if (az_g < UI_FLIP_G) {
        s_flip_hits++;
        if ((s_flip_hits >= UI_FLIP_HITS) && !s_debug_on) {
            s_flip_hits = 0U;
            (void)pet_core_post(PET_EVT_IMU_FLIP, 0);
        }
    } else {
        s_flip_hits = 0U;
    }

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
    }
#endif
}

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

static void ui_screen_pet_create(void)
{
    lv_obj_t *scr = lv_screen_active();

    pet_fs_set_root("/sdcard/pet");
    if (pet_res_load()) {
        pet_core_init(pet_res_needs_cfg());
        LOG_INFO("pet pack loaded root=%s", pet_fs_root());
    } else {
        pet_core_init(NULL);
        LOG_WARN("pet pack missing (%s); fallback body", pet_fs_root());
    }
    pet_view_set_alloc(ui_alloc_psram, ui_free_psram);
    pet_view_set_intent_hook(ui_intent_hook);
    pet_view_create(scr);
    ui_debug_overlay_create(scr);
    (void)lv_timer_create(ui_gesture_timer_cb, UI_GESTURE_PERIOD_MS, NULL);
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t delay_ms;

        if (s_debug_req != 0) {
            s_debug_req = 0;
            s_debug_on = !s_debug_on;
            ui_apply_debug_visible();
        }

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
    s_debug_req = 1;
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

    (void)gc9a01_set_backlight(lcd, true);
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
