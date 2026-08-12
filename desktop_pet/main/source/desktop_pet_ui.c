/**
 * @file desktop_pet_ui.c
 * @brief LVGL → GC9A01 + IT7259 触摸；第一页 owo + pet。
 */

#include "desktop_pet_ui.h"

#include "board.h"
#include "desktop_pet_audio.h"
#include "gc9a01.h"
#include "i2c.h"
#include "it7259.h"
#include "log.h"
#include "net_wifi.h"
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

#define UI_HOR_RES (240)
#define UI_VER_RES (240)
#define UI_BUF_LINES (40)
#define UI_TASK_STACK_WORDS (8192U)
#define UI_TASK_PRIORITY (4U)
#define UI_TICK_PERIOD_MS (1U)
#define UI_ANGLE_PERIOD_MS (100U)
#define UI_ACCEL_LSB_PER_G (16384.0f)   /* ±2g */
#define UI_GYRO_LSB_PER_DPS (16.0f)     /* ±2048 dps */
#define UI_RAD2DEG (57.2957795f)

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static uint8_t *s_buf1;
static uint8_t *s_buf2;
static esp_timer_handle_t s_tick_timer;
static bool s_started;
static lv_obj_t *s_lbl_roll;
static lv_obj_t *s_lbl_pitch;
static lv_obj_t *s_lbl_yaw;
static lv_obj_t *s_lbl_rec;
static lv_obj_t *s_lbl_ip;
static lv_obj_t *s_btn_rec_lbl;
static float s_yaw_deg;
static volatile int s_rec_req; /* 0=none 1=start 2=stop */

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
    /* NO_POINT / BUSY：无新报点时保持上次按下态，避免连点丢失 */

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
    /* 触摸方向按实测不再取反（与显示 MX 分开处理） */
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

static void ui_pet_btn_cb(lv_event_t *e)
{
    static unsigned face_i;
    static const char *const faces[] = {"owo", "^_^", "-_-", "O_O"};
    lv_obj_t *face = (lv_obj_t *)lv_event_get_user_data(e);

    if (face == NULL) {
        return;
    }
    face_i = (face_i + 1U) % (sizeof(faces) / sizeof(faces[0]));
    lv_label_set_text(face, faces[face_i]);
}

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
}

static void ui_rec_btn_cb(lv_event_t *e)
{
    (void)e;
    LOG_INFO("Rec button pressed (audio_ready=%d recording=%d)",
             desktop_pet_audio_is_ready() ? 1 : 0,
             desktop_pet_audio_is_recording() ? 1 : 0);

    if (!desktop_pet_audio_is_ready()) {
        ui_rec_status_set("Rec: N/A", false);
        LOG_WARN("Rec ignored: audio not ready");
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

static void ui_rec_poll_timer_cb(lv_timer_t *timer)
{
    char buf[40];

    (void)timer;

    if (s_rec_req == 1) {
        s_rec_req = 0;
        if (desktop_pet_audio_record_start() != STATUS_OK) {
            ui_rec_status_set("Rec fail", false);
            LOG_ERROR("audio record_start failed");
        } else {
            ui_rec_status_set("Recording...", true);
            LOG_INFO("audio record_start ok");
        }
    } else if (s_rec_req == 2) {
        char path[64];

        s_rec_req = 0;
        (void)desktop_pet_audio_record_stop();
        LOG_INFO("audio record_stop, bytes=%u", (unsigned)desktop_pet_audio_pcm_bytes());
        if (desktop_pet_audio_save_to_sd(path, sizeof(path)) == STATUS_OK) {
            (void)snprintf(buf, sizeof(buf), "Saved");
            ui_rec_status_set(buf, false);
            LOG_INFO("audio saved: %s", path);
        } else {
            (void)snprintf(buf, sizeof(buf), "Idle %uB", (unsigned)desktop_pet_audio_pcm_bytes());
            ui_rec_status_set(buf, false);
            LOG_WARN("audio save SD failed (card/FAT?)");
        }
    }

    if (!desktop_pet_audio_is_ready()) {
        return;
    }
    if (desktop_pet_audio_is_recording()) {
        (void)snprintf(buf, sizeof(buf), "REC %uB", (unsigned)desktop_pet_audio_pcm_bytes());
        ui_rec_status_set(buf, true);
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

static void ui_angle_timer_cb(lv_timer_t *timer)
{
    char buf[24];
    qmi8658a_t *imu;
    int16_t ax, ay, az, gx, gy, gz;
    float ax_g, ay_g, az_g;
    float roll, pitch;
    float dt = (float)UI_ANGLE_PERIOD_MS / 1000.0f;

    (void)timer;

#if !DESKTOP_PET_ENABLE_IMU
    if (s_lbl_roll != NULL) {
        lv_label_set_text(s_lbl_roll, "R: --");
        lv_label_set_text(s_lbl_pitch, "P: --");
        lv_label_set_text(s_lbl_yaw, "Y: --");
    }
    return;
#else
    imu = BoardQmi8658();
    if ((imu == NULL) || !imu->initialized) {
        if (s_lbl_roll != NULL) {
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

    roll = atan2f(ay_g, az_g) * UI_RAD2DEG;
    pitch = atan2f(-ax_g, sqrtf(ay_g * ay_g + az_g * az_g)) * UI_RAD2DEG;
    s_yaw_deg += ((float)gz / UI_GYRO_LSB_PER_DPS) * dt;
    if (s_yaw_deg > 180.0f) {
        s_yaw_deg -= 360.0f;
    } else if (s_yaw_deg < -180.0f) {
        s_yaw_deg += 360.0f;
    }

    if (s_lbl_roll != NULL) {
        (void)snprintf(buf, sizeof(buf), "R:%5.1f", (double)roll);
        lv_label_set_text(s_lbl_roll, buf);
        (void)snprintf(buf, sizeof(buf), "P:%5.1f", (double)pitch);
        lv_label_set_text(s_lbl_pitch, buf);
        (void)snprintf(buf, sizeof(buf), "Y:%5.1f", (double)s_yaw_deg);
        lv_label_set_text(s_lbl_yaw, buf);
    }
#endif
}

static void ui_screen_home_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *face;
    lv_obj_t *btn;
    lv_obj_t *btn_lbl;
    lv_obj_t *btn_rec;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x202020), 0);

    /* 无背景色的三轴角度（R/P 加速度姿态，Y 为陀螺 Z 积分） */
    s_lbl_roll = ui_angle_label_create(scr, "R:  0.0", 18);
    s_lbl_pitch = ui_angle_label_create(scr, "P:  0.0", 36);
    s_lbl_yaw = ui_angle_label_create(scr, "Y:  0.0", 54);
    (void)lv_timer_create(ui_angle_timer_cb, UI_ANGLE_PERIOD_MS, NULL);

    s_lbl_rec = ui_angle_label_create(scr, "Idle", 72);
    (void)lv_timer_create(ui_rec_poll_timer_cb, 500, NULL);

    /* 底部无背景 IP（STA 优先，否则 SoftAP） */
    s_lbl_ip = lv_label_create(scr);
    lv_label_set_text(s_lbl_ip, "IP: ---");
    lv_obj_set_style_bg_opa(s_lbl_ip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_lbl_ip, 0, 0);
    lv_obj_set_style_pad_all(s_lbl_ip, 0, 0);
    lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(0xA0E0FF), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_lbl_ip, &lv_font_montserrat_14, 0);
#endif
    lv_obj_align(s_lbl_ip, LV_ALIGN_BOTTOM_MID, 0, -18);
    (void)lv_timer_create(ui_ip_timer_cb, 1000, NULL);
    ui_ip_timer_cb(NULL);

    face = lv_label_create(scr);
    lv_label_set_text(face, "owo");
#if LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(face, &lv_font_montserrat_24, 0);
#endif
    lv_obj_set_style_text_color(face, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(face, LV_ALIGN_CENTER, 0, -20);

    btn = lv_button_create(scr);
    lv_obj_set_size(btn, 80, 40);
    lv_obj_align(btn, LV_ALIGN_CENTER, -48, 40);
    lv_obj_add_event_cb(btn, ui_pet_btn_cb, LV_EVENT_CLICKED, face);
    btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "pet");
    lv_obj_center(btn_lbl);

    btn_rec = lv_button_create(scr);
    lv_obj_set_size(btn_rec, 80, 40);
    lv_obj_align(btn_rec, LV_ALIGN_CENTER, 48, 40);
    lv_obj_add_flag(btn_rec, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_rec, ui_rec_btn_cb, LV_EVENT_PRESSED, NULL);
    s_btn_rec_lbl = lv_label_create(btn_rec);
    lv_label_set_text(s_btn_rec_lbl, "Rec");
    lv_obj_center(s_btn_rec_lbl);

    if (!desktop_pet_audio_is_ready()) {
        ui_rec_status_set("Rec: N/A", false);
    } else {
        ui_rec_status_set("Idle", false);
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t delay_ms = lv_timer_handler();

        if (delay_ms > 50U) {
            delay_ms = 50U;
        }
        if (delay_ms < 1U) {
            delay_ms = 1U;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void *ui_alloc_buf(size_t nbytes)
{
    void *p = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (p == NULL) {
        p = heap_caps_malloc(nbytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
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
        return STATUS_FAIL;
    }

    s_disp = lv_display_create(UI_HOR_RES, UI_VER_RES);
    if (s_disp == NULL) {
        LOG_ERROR("desktop_pet_ui: lv_display_create failed");
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
        return STATUS_FAIL;
    }
    if (esp_timer_start_periodic(s_tick_timer, UI_TICK_PERIOD_MS * 1000ULL) != ESP_OK) {
        LOG_ERROR("desktop_pet_ui: tick timer start failed");
        return STATUS_FAIL;
    }

#if DESKTOP_PET_ENABLE_AUDIO
    if (desktop_pet_audio_init() != STATUS_OK) {
        LOG_WARN("desktop_pet_audio_init failed (Rec button limited)");
    }
#endif

    ui_screen_home_create();

    if (xTaskCreate(ui_task, "pet_ui", UI_TASK_STACK_WORDS, NULL, UI_TASK_PRIORITY, NULL) != pdPASS) {
        LOG_ERROR("desktop_pet_ui: task create failed");
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
