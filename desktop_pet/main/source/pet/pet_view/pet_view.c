/**
 * @file pet_view.c
 * @brief Round-screen compositor: body, face, Needs dots, Dock (product §B).
 */

#include "pet_view.h"

#include "pet_core.h"
#include "pet_res.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PET_VIEW_POLL_MS (100U)
#define PET_VIEW_TICK_DIV (10U)
#define PET_VIEW_BLINK_PERIOD_MS (3200U)
#define PET_VIEW_BLINK_MS (140U)
#define PET_VIEW_HOLD_MS (650U)
#define PET_BODY_PIXELS ((uint32_t)PET_RES_FRAME_MAX_W * (uint32_t)PET_RES_FRAME_MAX_H)
#define PET_SPLASH_PIXELS ((uint32_t)PET_RES_SPLASH_MAX_W * (uint32_t)PET_RES_SPLASH_MAX_H)
#define PET_SPLASH_GATE_MS (1000U)
#define PET_SPLASH_POLL_MS (50U)
#define PET_SPLASH_ARC_SIZE (132)
#define PET_SPLASH_FALLBACK_SIZE (96)
#define PET_NEED_DOT_SIZE (10)
#define PET_NEED_DOT_GAP (8)
#define PET_DOCK_BTN_SIZE (28)
#define PET_DOCK_BTN_GAP (8)
#define PET_HINT_FADE_MS (2800U)

static pet_view_alloc_fn s_alloc;
static pet_view_free_fn s_free;
static pet_view_intent_hook_t s_intent_hook;
static pet_view_boot_done_fn s_boot_done;
static lv_obj_t *s_boot_parent;
static lv_obj_t *s_splash_layer;
static lv_obj_t *s_splash_img;
static lv_obj_t *s_splash_fallback;
static lv_obj_t *s_splash_arc;
static lv_image_dsc_t s_splash_dsc;
static uint16_t *s_splash_pix;
static lv_timer_t *s_splash_timer;
static uint32_t s_splash_t0;
static bool s_splash_pack_done;
static bool s_splash_active;
static int16_t s_splash_arc_rot;

static lv_obj_t *s_body_fallback;
static lv_obj_t *s_body_img;
static lv_obj_t *s_eye_l;
static lv_obj_t *s_eye_r;
static lv_obj_t *s_pupil_l;
static lv_obj_t *s_pupil_r;
static lv_obj_t *s_mouth;
static lv_obj_t *s_brow_l;
static lv_obj_t *s_brow_r;
static lv_obj_t *s_need_h;
static lv_obj_t *s_need_m;
static lv_obj_t *s_need_e;
static lv_obj_t *s_pack_hint;
static lv_timer_t *s_hint_timer;

static lv_image_dsc_t s_img_dsc[2];
static uint16_t *s_pix[2];
static uint8_t s_pix_i;
static uint16_t s_frame_w;
static uint16_t s_frame_h;
static pet_clip_id_t s_shown_clip;
static uint8_t s_frame_idx;
static uint32_t s_frame_acc_ms;
static pet_face_id_t s_shown_face;
static uint8_t s_tick_div;
static uint32_t s_blink_acc;
static bool s_blinking;
static bool s_pressing;
static uint32_t s_press_ms;
static bool s_hold_sent;

static void *view_alloc(size_t n)
{
    if (s_alloc != NULL) {
        return s_alloc(n);
    }
    return malloc(n);
}

static void view_free(void *p)
{
    if (p == NULL) {
        return;
    }
    if (s_free != NULL) {
        s_free(p);
        return;
    }
    free(p);
}

static void fill_img_dsc(lv_image_dsc_t *dsc, const uint16_t *pix, uint16_t w, uint16_t h)
{
    (void)memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = (uint16_t)(w * 2U);
    dsc->data_size = (uint32_t)w * (uint32_t)h * 2U;
    dsc->data = (const uint8_t *)pix;
}

static void set_need_dot(lv_obj_t *dot, uint8_t v, uint32_t color_ok, uint32_t color_low)
{
    lv_opa_t opa;

    if (dot == NULL) {
        return;
    }
    /* Brightness tracks level; low value switches to warn tint. */
    if (v < 25U) {
        lv_obj_set_style_bg_color(dot, lv_color_hex(color_low), 0);
        opa = (lv_opa_t)(LV_OPA_50 + ((uint16_t)v * (LV_OPA_COVER - LV_OPA_50)) / 25U);
    } else {
        lv_obj_set_style_bg_color(dot, lv_color_hex(color_ok), 0);
        opa = (lv_opa_t)(LV_OPA_60 + (((uint16_t)v - 25U) * (LV_OPA_COVER - LV_OPA_60)) / 75U);
    }
    lv_obj_set_style_bg_opa(dot, opa, 0);
}

static void apply_hud(void)
{
    pet_needs_t n;

    pet_core_get_needs(&n);
    set_need_dot(s_need_h, n.hunger, 0x7CFF9AU, 0xFF6666U);
    set_need_dot(s_need_m, n.mood, 0x7EC8FFU, 0xFFAA44U);
    set_need_dot(s_need_e, n.energy, 0xFFE27AU, 0xC0C0C0U);
}

static void apply_face(pet_face_id_t face)
{
    int32_t eye_h = 22;
    int32_t pupil_y = 0;
    int32_t mouth_w = 28;
    int32_t mouth_h = 8;
    lv_color_t eye_c = lv_color_hex(0xFFFFFF);
    lv_color_t mouth_c = lv_color_hex(0xE07080);
    bool brows = false;

    s_shown_face = face;
    switch (face) {
    case PET_FACE_HAPPY:
        mouth_w = 36;
        mouth_h = 12;
        mouth_c = lv_color_hex(0xFF8A9A);
        break;
    case PET_FACE_SAD:
        mouth_w = 22;
        mouth_h = 5;
        pupil_y = 3;
        mouth_c = lv_color_hex(0x886688);
        break;
    case PET_FACE_SLEEPY:
        eye_h = 6;
        pupil_y = 0;
        mouth_w = 18;
        mouth_h = 4;
        break;
    case PET_FACE_HUNGRY:
        mouth_w = 16;
        mouth_h = 14;
        mouth_c = lv_color_hex(0xCC4466);
        break;
    case PET_FACE_ANGRY:
        brows = true;
        eye_c = lv_color_hex(0xFFE8E8);
        mouth_w = 20;
        mouth_h = 6;
        break;
    default:
        break;
    }

    if (s_eye_l != NULL) {
        lv_obj_set_height(s_eye_l, eye_h);
        lv_obj_set_height(s_eye_r, eye_h);
        lv_obj_set_style_bg_color(s_eye_l, eye_c, 0);
        lv_obj_set_style_bg_color(s_eye_r, eye_c, 0);
    }
    if (s_pupil_l != NULL) {
        lv_obj_set_y(s_pupil_l, pupil_y);
        lv_obj_set_y(s_pupil_r, pupil_y);
    }
    if (s_mouth != NULL) {
        lv_obj_set_size(s_mouth, mouth_w, mouth_h);
        lv_obj_set_style_bg_color(s_mouth, mouth_c, 0);
        lv_obj_set_style_radius(s_mouth, mouth_h / 2 + 2, 0);
        lv_obj_align(s_mouth, LV_ALIGN_CENTER, 0, 28);
    }
    if (s_brow_l != NULL) {
        if (brows) {
            lv_obj_remove_flag(s_brow_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_brow_r, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_brow_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_brow_r, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static bool load_frame_to(uint8_t slot, pet_clip_id_t clip, uint8_t fi)
{
    uint16_t w = 0;
    uint16_t h = 0;

    if ((s_pix[slot] == NULL) || !pet_res_is_loaded()) {
        return false;
    }
    if (!pet_res_load_frame(clip, fi, s_pix[slot], PET_BODY_PIXELS, &w, &h)) {
        return false;
    }
    s_frame_w = w;
    s_frame_h = h;
    fill_img_dsc(&s_img_dsc[slot], s_pix[slot], w, h);
    return true;
}

static void show_body_clip(pet_clip_id_t clip)
{
    uint8_t n;

    s_shown_clip = clip;
    s_frame_idx = 0U;
    s_frame_acc_ms = 0U;
    n = pet_res_clip_frame_count(clip);
    if ((n == 0U) || (s_body_img == NULL) || (s_pix[0] == NULL)) {
        if (s_body_img != NULL) {
            lv_obj_add_flag(s_body_img, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_body_fallback != NULL) {
            lv_obj_remove_flag(s_body_fallback, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (!load_frame_to(0, clip, 0U)) {
        return;
    }
    s_pix_i = 0U;
    lv_image_set_src(s_body_img, &s_img_dsc[0]);
    lv_obj_set_size(s_body_img, (int32_t)s_frame_w, (int32_t)s_frame_h);
    lv_obj_remove_flag(s_body_img, LV_OBJ_FLAG_HIDDEN);
    if (s_body_fallback != NULL) {
        lv_obj_add_flag(s_body_fallback, LV_OBJ_FLAG_HIDDEN);
    }
}

static void advance_frame(void)
{
    uint8_t n = pet_res_clip_frame_count(s_shown_clip);
    uint8_t fps = pet_res_clip_fps(s_shown_clip);
    uint8_t next;
    uint8_t slot;

    if ((n < 2U) || (fps == 0U) || (s_body_img == NULL)) {
        return;
    }
    s_frame_acc_ms += PET_VIEW_POLL_MS;
    if (s_frame_acc_ms < (1000U / (uint32_t)fps)) {
        return;
    }
    s_frame_acc_ms = 0U;
    next = (uint8_t)((s_frame_idx + 1U) % n);
    slot = (uint8_t)(1U - s_pix_i);
    if (!load_frame_to(slot, s_shown_clip, next)) {
        return;
    }
    s_pix_i = slot;
    s_frame_idx = next;
    lv_image_set_src(s_body_img, &s_img_dsc[slot]);
}

static void apply_blink(bool closed)
{
    if ((s_shown_face == PET_FACE_SLEEPY) || (s_eye_l == NULL)) {
        return;
    }
    lv_obj_set_height(s_eye_l, closed ? 3 : 22);
    lv_obj_set_height(s_eye_r, closed ? 3 : 22);
}

static void drain_intents(void)
{
    pet_intent_t in;

    while (pet_core_take_intent(&in)) {
        switch (in.id) {
        case PET_INTENT_CLIP:
            show_body_clip((pet_clip_id_t)in.arg0);
            apply_hud();
            break;
        case PET_INTENT_FACE:
            apply_face((pet_face_id_t)in.arg0);
            apply_hud();
            break;
        case PET_INTENT_HUD:
            apply_hud();
            break;
        default:
            if (s_intent_hook != NULL) {
                s_intent_hook(&in);
            }
            break;
        }
    }
}

static void care_cb(lv_event_t *e)
{
    pet_evt_id_t id = (pet_evt_id_t)(uintptr_t)lv_event_get_user_data(e);

    (void)pet_core_post(id, 0);
}

static void chat_cb(lv_event_t *e)
{
    pet_intent_t in;

    (void)e;
    in.id = PET_INTENT_OPEN_CHAT;
    in.arg0 = 0;
    in.arg1 = 0;
    if (s_intent_hook != NULL) {
        s_intent_hook(&in);
    }
}

static void body_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_pressing = true;
        s_press_ms = 0U;
        s_hold_sent = false;
    } else if (code == LV_EVENT_RELEASED) {
        if (s_pressing && !s_hold_sent) {
            (void)pet_core_post(PET_EVT_TOUCH_TAP, 0);
        }
        s_pressing = false;
        s_hold_sent = false;
    }
}

static lv_obj_t *make_need_dot(lv_obj_t *parent, int32_t x_ofs, uint32_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);

    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, PET_NEED_DOT_SIZE, PET_NEED_DOT_SIZE);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_opa(dot, LV_OPA_40, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, x_ofs, 20);
    return dot;
}

static lv_obj_t *make_dock_btn(lv_obj_t *parent, const char *title, int32_t x, pet_evt_id_t evt,
                               bool chat_style)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl;

    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, PET_DOCK_BTN_SIZE, PET_DOCK_BTN_SIZE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (chat_style) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A5570), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x7EC8FF), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_70, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A44), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    }
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x, -14);
    if (chat_style) {
        lv_obj_add_event_cb(btn, chat_cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_add_event_cb(btn, care_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)evt);
    }
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(chat_style ? 0xDCEEFFU : 0xE8EAF0U), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
#endif
    lv_obj_center(lbl);
    return btn;
}

static void hint_fade_cb(lv_timer_t *t)
{
    (void)t;
    if (s_pack_hint != NULL) {
        lv_obj_add_flag(s_pack_hint, LV_OBJ_FLAG_HIDDEN);
    }
    s_hint_timer = NULL;
}

static lv_obj_t *make_eye(lv_obj_t *parent, int32_t x)
{
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_t *pupil;

    lv_obj_remove_style_all(eye);
    lv_obj_set_size(eye, 18, 22);
    lv_obj_set_style_bg_color(eye, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(eye, LV_ALIGN_CENTER, x, -8);

    pupil = lv_obj_create(eye);
    lv_obj_remove_style_all(pupil);
    lv_obj_set_size(pupil, 8, 10);
    lv_obj_set_style_bg_color(pupil, lv_color_hex(0x202028), 0);
    lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(pupil);
    if (x < 0) {
        s_pupil_l = pupil;
    } else {
        s_pupil_r = pupil;
    }
    return eye;
}

static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    pet_view_poll();
}

static void splash_destroy(void)
{
    if (s_splash_timer != NULL) {
        lv_timer_delete(s_splash_timer);
        s_splash_timer = NULL;
    }
    if (s_splash_layer != NULL) {
        lv_obj_delete(s_splash_layer);
        s_splash_layer = NULL;
    }
    s_splash_img = NULL;
    s_splash_fallback = NULL;
    s_splash_arc = NULL;
    if (s_splash_pix != NULL) {
        view_free(s_splash_pix);
        s_splash_pix = NULL;
    }
    (void)memset(&s_splash_dsc, 0, sizeof(s_splash_dsc));
    s_splash_active = false;
}

static void splash_enter_home(void)
{
    lv_obj_t *parent = s_boot_parent;
    pet_view_boot_done_fn done = s_boot_done;

    splash_destroy();
    pet_view_create(parent);
    if (done != NULL) {
        done(parent);
    }
}

static void splash_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_splash_active) {
        return;
    }

    /* Spin the progress ring (visual C). */
    if (s_splash_arc != NULL) {
        s_splash_arc_rot = (int16_t)((s_splash_arc_rot + 12) % 360);
        lv_arc_set_rotation(s_splash_arc, (int32_t)s_splash_arc_rot);
    }

    if (!s_splash_pack_done) {
        return;
    }
    if (lv_tick_elaps(s_splash_t0) < PET_SPLASH_GATE_MS) {
        return;
    }
    splash_enter_home();
}

void pet_view_set_alloc(pet_view_alloc_fn alloc_fn, pet_view_free_fn free_fn)
{
    s_alloc = alloc_fn;
    s_free = free_fn;
}

void pet_view_set_intent_hook(pet_view_intent_hook_t hook)
{
    s_intent_hook = hook;
}

void pet_view_boot_start(lv_obj_t *parent, pet_view_boot_done_fn on_home)
{
    uint16_t w = 0;
    uint16_t h = 0;
    bool got_splash;

    if (parent == NULL) {
        parent = lv_screen_active();
    }
    s_boot_parent = parent;
    s_boot_done = on_home;
    s_splash_pack_done = false;
    s_splash_arc_rot = 270;
    s_splash_t0 = lv_tick_get();
    s_splash_active = true;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x202020), 0);

    s_splash_layer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_splash_layer);
    lv_obj_set_size(s_splash_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_splash_layer, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(s_splash_layer, LV_OPA_COVER, 0);
    lv_obj_align(s_splash_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_splash_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_splash_pix = (uint16_t *)view_alloc(PET_SPLASH_PIXELS * 2U);
    got_splash = false;
    if (s_splash_pix != NULL) {
        got_splash = pet_res_load_splash(s_splash_pix, PET_SPLASH_PIXELS, &w, &h);
    }

    if (got_splash) {
        fill_img_dsc(&s_splash_dsc, s_splash_pix, w, h);
        s_splash_img = lv_image_create(s_splash_layer);
        lv_image_set_src(s_splash_img, &s_splash_dsc);
        lv_obj_set_size(s_splash_img, (int32_t)w, (int32_t)h);
        lv_obj_align(s_splash_img, LV_ALIGN_CENTER, 0, 0);
    } else {
        s_splash_fallback = lv_obj_create(s_splash_layer);
        lv_obj_remove_style_all(s_splash_fallback);
        lv_obj_set_size(s_splash_fallback, PET_SPLASH_FALLBACK_SIZE, PET_SPLASH_FALLBACK_SIZE);
        lv_obj_set_style_bg_color(s_splash_fallback, lv_color_hex(0x4AA3C8), 0);
        lv_obj_set_style_bg_opa(s_splash_fallback, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_splash_fallback, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(s_splash_fallback, LV_ALIGN_CENTER, 0, -8);
        if (s_splash_pix != NULL) {
            view_free(s_splash_pix);
            s_splash_pix = NULL;
        }
    }

    /* Arc above splash bitmap (visual C). */
    s_splash_arc = lv_arc_create(s_splash_layer);
    lv_obj_set_size(s_splash_arc, PET_SPLASH_ARC_SIZE, PET_SPLASH_ARC_SIZE);
    lv_obj_align(s_splash_arc, LV_ALIGN_CENTER, 0, -8);
    lv_arc_set_bg_angles(s_splash_arc, 0, 360);
    lv_arc_set_angles(s_splash_arc, 0, 270);
    lv_arc_set_rotation(s_splash_arc, s_splash_arc_rot);
    lv_obj_remove_style(s_splash_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_splash_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_splash_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_splash_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_splash_arc, lv_color_hex(0x2A3148), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_splash_arc, lv_color_hex(0x7EC8FF), LV_PART_INDICATOR);

    s_splash_timer = lv_timer_create(splash_timer_cb, PET_SPLASH_POLL_MS, NULL);
}

void pet_view_boot_pack_done(void)
{
    s_splash_pack_done = true;
    /* If boot_start was skipped, open home immediately. */
    if (!s_splash_active && (s_boot_parent != NULL)) {
        splash_enter_home();
    }
}

void pet_view_create(lv_obj_t *parent)
{
    lv_obj_t *hit;
    lv_obj_t *dock;
    int32_t dock_span;
    uint32_t bytes = PET_BODY_PIXELS * 2U;

    if (parent == NULL) {
        parent = lv_screen_active();
    }
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x202020), 0);

    s_pix[0] = (uint16_t *)view_alloc(bytes);
    s_pix[1] = (uint16_t *)view_alloc(bytes);
    if ((s_pix[0] == NULL) || (s_pix[1] == NULL)) {
        view_free(s_pix[0]);
        view_free(s_pix[1]);
        s_pix[0] = NULL;
        s_pix[1] = NULL;
    }

    s_body_fallback = lv_obj_create(parent);
    lv_obj_remove_style_all(s_body_fallback);
    lv_obj_set_size(s_body_fallback, 150, 150);
    lv_obj_set_style_bg_color(s_body_fallback, lv_color_hex(0x4AA3C8), 0);
    lv_obj_set_style_bg_opa(s_body_fallback, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_body_fallback, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(s_body_fallback, LV_ALIGN_CENTER, 0, -6);

    s_body_img = lv_image_create(parent);
    lv_obj_align(s_body_img, LV_ALIGN_CENTER, 0, -6);
    lv_obj_add_flag(s_body_img, LV_OBJ_FLAG_HIDDEN);

    s_eye_l = make_eye(parent, -18);
    s_eye_r = make_eye(parent, 18);

    s_brow_l = lv_obj_create(parent);
    lv_obj_remove_style_all(s_brow_l);
    lv_obj_set_size(s_brow_l, 16, 3);
    lv_obj_set_style_bg_color(s_brow_l, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(s_brow_l, LV_OPA_COVER, 0);
    lv_obj_align(s_brow_l, LV_ALIGN_CENTER, -18, -24);
    lv_obj_add_flag(s_brow_l, LV_OBJ_FLAG_HIDDEN);

    s_brow_r = lv_obj_create(parent);
    lv_obj_remove_style_all(s_brow_r);
    lv_obj_set_size(s_brow_r, 16, 3);
    lv_obj_set_style_bg_color(s_brow_r, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(s_brow_r, LV_OPA_COVER, 0);
    lv_obj_align(s_brow_r, LV_ALIGN_CENTER, 18, -24);
    lv_obj_add_flag(s_brow_r, LV_OBJ_FLAG_HIDDEN);

    s_mouth = lv_obj_create(parent);
    lv_obj_remove_style_all(s_mouth);
    lv_obj_set_size(s_mouth, 28, 8);
    lv_obj_set_style_bg_color(s_mouth, lv_color_hex(0xE07080), 0);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mouth, 6, 0);
    lv_obj_align(s_mouth, LV_ALIGN_CENTER, 0, 28);

    hit = lv_obj_create(parent);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, 150, 150);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_align(hit, LV_ALIGN_CENTER, 0, -6);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, body_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hit, body_event_cb, LV_EVENT_RELEASED, NULL);

    /* Needs：顶中三点，间距 8，绿/蓝/黄 = 饥饿/心情/精力 */
    {
        int32_t step = PET_NEED_DOT_SIZE + PET_NEED_DOT_GAP;
        s_need_h = make_need_dot(parent, -step, 0x7CFF9A);
        s_need_m = make_need_dot(parent, 0, 0x7EC8FF);
        s_need_e = make_need_dot(parent, step, 0xFFE27A);
    }

    s_pack_hint = lv_label_create(parent);
    lv_obj_set_style_text_color(s_pack_hint, lv_color_hex(0xA0A0A0), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_pack_hint, &lv_font_montserrat_14, 0);
#endif
    lv_obj_align(s_pack_hint, LV_ALIGN_TOP_MID, 0, 36);
    if (pet_res_is_loaded()) {
        lv_label_set_text(s_pack_hint, "");
        lv_obj_add_flag(s_pack_hint, LV_OBJ_FLAG_HIDDEN);
        show_body_clip(PET_CLIP_IDLE);
    } else {
        lv_label_set_text(s_pack_hint, "NO PACK");
        s_hint_timer = lv_timer_create(hint_fade_cb, PET_HINT_FADE_MS, NULL);
        lv_timer_set_repeat_count(s_hint_timer, 1);
    }

    /* Dock 底弧四钮：喂/玩/睡/聊；ø28 间距 8（无中文字体用 F/P/S/C） */
    dock_span = PET_DOCK_BTN_SIZE + PET_DOCK_BTN_GAP;
    dock = lv_obj_create(parent);
    lv_obj_remove_style_all(dock);
    lv_obj_set_size(dock, (4 * PET_DOCK_BTN_SIZE) + (3 * PET_DOCK_BTN_GAP) + 16,
                    PET_DOCK_BTN_SIZE + 10);
    lv_obj_set_style_bg_color(dock, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_30, 0);
    lv_obj_set_style_radius(dock, 999, 0);
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -9);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_CLICKABLE);

    (void)make_dock_btn(parent, "F", (int32_t)(-3 * dock_span / 2), PET_EVT_CARE_FEED, false);
    (void)make_dock_btn(parent, "P", (int32_t)(-dock_span / 2), PET_EVT_CARE_PLAY, false);
    (void)make_dock_btn(parent, "S", (int32_t)(dock_span / 2), PET_EVT_CARE_SLEEP, false);
    (void)make_dock_btn(parent, "C", (int32_t)(3 * dock_span / 2), PET_EVT_CARE_WAKE, true);

    apply_face(PET_FACE_IDLE);
    apply_hud();
    drain_intents();
    (void)lv_timer_create(poll_timer_cb, PET_VIEW_POLL_MS, NULL);
}

void pet_view_poll(void)
{
    pet_core_poll();
    drain_intents();
    advance_frame();

    s_tick_div++;
    if (s_tick_div >= PET_VIEW_TICK_DIV) {
        s_tick_div = 0U;
        (void)pet_core_post(PET_EVT_TICK_1S, 0);
        pet_core_poll();
        drain_intents();
    }

    if (s_pressing) {
        s_press_ms += PET_VIEW_POLL_MS;
        if (!s_hold_sent && (s_press_ms >= PET_VIEW_HOLD_MS)) {
            s_hold_sent = true;
            (void)pet_core_post(PET_EVT_TOUCH_HOLD, 0);
        }
    }

    s_blink_acc += PET_VIEW_POLL_MS;
    if (!s_blinking && (s_blink_acc >= PET_VIEW_BLINK_PERIOD_MS)) {
        s_blinking = true;
        s_blink_acc = 0U;
        apply_blink(true);
    } else if (s_blinking && (s_blink_acc >= PET_VIEW_BLINK_MS)) {
        s_blinking = false;
        s_blink_acc = 0U;
        apply_blink(false);
        apply_face(s_shown_face);
    }
}
