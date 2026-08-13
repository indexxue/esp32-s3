/**
 * @file pet_view.c
 * @brief Round-screen compositor: body image, eyes/mouth, HUD, care buttons.
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
#define PET_BAR_W (56)
#define PET_BAR_H (6)

static pet_view_alloc_fn s_alloc;
static pet_view_free_fn s_free;
static pet_view_intent_hook_t s_intent_hook;

static lv_obj_t *s_body_fallback;
static lv_obj_t *s_body_img;
static lv_obj_t *s_eye_l;
static lv_obj_t *s_eye_r;
static lv_obj_t *s_pupil_l;
static lv_obj_t *s_pupil_r;
static lv_obj_t *s_mouth;
static lv_obj_t *s_brow_l;
static lv_obj_t *s_brow_r;
static lv_obj_t *s_bar_h;
static lv_obj_t *s_bar_m;
static lv_obj_t *s_bar_e;
static lv_obj_t *s_status;
static lv_obj_t *s_pack_hint;

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

static void set_bar(lv_obj_t *bar, uint8_t v, uint32_t color)
{
    int32_t w;

    if (bar == NULL) {
        return;
    }
    w = ((int32_t)PET_BAR_W * (int32_t)v) / 100;
    if (w < 2) {
        w = 2;
    }
    lv_obj_set_width(bar, w);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
}

static void apply_hud(void)
{
    pet_needs_t n;
    const char *st = "";

    pet_core_get_needs(&n);
    set_bar(s_bar_h, n.hunger, n.hunger < 25U ? 0xFF6666U : 0x7CFF9AU);
    set_bar(s_bar_m, n.mood, n.mood < 25U ? 0xFFAA44U : 0x7EC8FFU);
    set_bar(s_bar_e, n.energy, n.energy < 25U ? 0xC0C0C0U : 0xFFE27AU);

    if (n.sleeping) {
        st = "zzz";
    } else if (pet_core_clip() == PET_CLIP_EAT) {
        st = "nom";
    } else if (pet_core_clip() == PET_CLIP_PLAY) {
        st = "yay";
    } else if (n.hunger < 25U) {
        st = "hungry";
    } else if (n.energy < 25U) {
        st = "sleepy";
    } else if (n.mood < 25U) {
        st = "lonely";
    } else {
        st = pet_face_name(pet_core_face());
    }
    if (s_status != NULL) {
        lv_label_set_text(s_status, st);
    }
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

static lv_obj_t *make_bar_track(lv_obj_t *parent, int32_t x, int32_t y, uint32_t col)
{
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_t *fill;

    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, PET_BAR_W, PET_BAR_H);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x404040), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_align(track, LV_ALIGN_TOP_MID, x, y);

    fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, PET_BAR_W, PET_BAR_H);
    lv_obj_set_style_bg_color(fill, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, 3, 0);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    return fill;
}

static lv_obj_t *make_care_btn(lv_obj_t *parent, const char *title, int32_t x, pet_evt_id_t evt)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl;

    lv_obj_set_size(btn, 56, 26);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x, -10);
    lv_obj_add_event_cb(btn, care_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)evt);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_center(lbl);
    return btn;
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

void pet_view_set_alloc(pet_view_alloc_fn alloc_fn, pet_view_free_fn free_fn)
{
    s_alloc = alloc_fn;
    s_free = free_fn;
}

void pet_view_set_intent_hook(pet_view_intent_hook_t hook)
{
    s_intent_hook = hook;
}

void pet_view_create(lv_obj_t *parent)
{
    lv_obj_t *hit;
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
    lv_obj_set_size(s_body_fallback, 120, 120);
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
    lv_obj_set_size(hit, 140, 140);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_align(hit, LV_ALIGN_CENTER, 0, -6);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, body_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hit, body_event_cb, LV_EVENT_RELEASED, NULL);

    s_bar_h = make_bar_track(parent, -62, 14, 0x7CFF9A);
    s_bar_m = make_bar_track(parent, 0, 14, 0x7EC8FF);
    s_bar_e = make_bar_track(parent, 62, 14, 0xFFE27A);

    s_status = lv_label_create(parent);
    lv_label_set_text(s_status, "idle");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xE8E8E8), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
#endif
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 26);

    s_pack_hint = lv_label_create(parent);
    lv_obj_set_style_text_color(s_pack_hint, lv_color_hex(0x808080), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_pack_hint, &lv_font_montserrat_14, 0);
#endif
    lv_obj_align(s_pack_hint, LV_ALIGN_TOP_MID, 0, 44);
    if (pet_res_is_loaded()) {
        lv_label_set_text(s_pack_hint, "");
        show_body_clip(PET_CLIP_IDLE);
    } else {
        lv_label_set_text(s_pack_hint, "NO PACK");
    }

    (void)make_care_btn(parent, "Feed", -60, PET_EVT_CARE_FEED);
    (void)make_care_btn(parent, "Play", 0, PET_EVT_CARE_PLAY);
    (void)make_care_btn(parent, "Sleep", 60, PET_EVT_CARE_SLEEP);

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
