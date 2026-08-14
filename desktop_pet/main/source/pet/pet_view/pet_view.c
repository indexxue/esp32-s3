/**
 * @file pet_view.c
 * @brief Round-screen compositor: body, Needs, left care arc, right Chat.
 *        Chat surface D: tap toggle listen/wait overlay.
 *        Face overlay is reserved: PET_VIEW_ENABLE_FACE=0 does not draw 五官.
 */

#include "pet_view.h"

#include "pet_core.h"
#include "pet_fs.h"
#include "pet_res.h"
#include "log.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PET_VIEW_POLL_MS (100U)
#define PET_VIEW_TICK_DIV (10U)
#ifndef PET_VIEW_ENABLE_FACE
#define PET_VIEW_ENABLE_FACE 0 /* product: 五官/表情预留，固件不画 */
#endif
#if PET_VIEW_ENABLE_FACE
#define PET_VIEW_BLINK_PERIOD_MS (3200U)
#define PET_VIEW_BLINK_MS (140U)
#endif
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
/* Body 热区收窄，给左侧护理弧 / 右侧 Chat 留空 */
#define PET_BODY_HIT_W (120)
#define PET_BODY_HIT_H (118)
#define PET_BODY_HIT_Y (-22)
#define PET_DOCK_BTN_EXT_CLICK (16)
/* 左侧护理弧：相对圆心偏移（中钮更靠外，呈弧） */
#define PET_CARE_ARC_X_MID (-104)
#define PET_CARE_ARC_X_WING (-84)
#define PET_CARE_ARC_Y (62)
#define PET_UI_ICON_PIXELS \
    ((uint32_t)PET_RES_UI_ICON_MAX_W * (uint32_t)PET_RES_UI_ICON_MAX_H)
#define PET_HINT_FADE_MS (2800U)
#define PET_CHAT_IDLE_MS (45000U)
#define PET_CHAT_WAVE_BARS (5)
/* 字幕条：CJK 14 行高约 17，双行 + pad */
#define PET_CHAT_CAP_BOX_W (196)
#define PET_CHAT_CAP_BOX_H (56)
#define PET_CHAT_CAP_LABEL_W (180)
#define PET_CHAT_CAPTION_MAX (160)

static pet_view_alloc_fn s_alloc;
static pet_view_free_fn s_free;
static pet_view_intent_hook_t s_intent_hook;
static pet_view_chat_hook_t s_chat_hook;
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
#if PET_VIEW_ENABLE_FACE
static lv_obj_t *s_eye_l;
static lv_obj_t *s_eye_r;
static lv_obj_t *s_pupil_l;
static lv_obj_t *s_pupil_r;
static lv_obj_t *s_mouth;
static lv_obj_t *s_brow_l;
static lv_obj_t *s_brow_r;
#endif
static lv_obj_t *s_need_h;
static lv_obj_t *s_need_m;
static lv_obj_t *s_need_e;
static lv_obj_t *s_pack_hint;
static lv_timer_t *s_hint_timer;
static lv_obj_t *s_body_hit;
static lv_obj_t *s_care_f;
static lv_obj_t *s_care_p;
static lv_obj_t *s_care_s;
static lv_obj_t *s_chat_btn;
static lv_obj_t *s_home_parent;

static lv_obj_t *s_chat_layer;
static lv_obj_t *s_chat_back;
static lv_obj_t *s_chat_mode;
static lv_obj_t *s_chat_caption;
static lv_obj_t *s_chat_cap_box;
static lv_obj_t *s_chat_wave[PET_CHAT_WAVE_BARS];
static bool s_chat_open;
static bool s_chat_listen_on;
static bool s_chat_listen_ui_pending;
static pet_chat_mode_t s_chat_mode_id;
static uint32_t s_chat_idle_ms;
static uint8_t s_chat_wave_phase;
static char s_chat_caption_buf[PET_CHAT_CAPTION_MAX];
static lv_font_t *s_sd_caption_font;

static lv_image_dsc_t s_img_dsc[2];
static uint16_t *s_pix[2];
static uint8_t s_pix_i;
static uint16_t s_frame_w;
static uint16_t s_frame_h;
static pet_clip_id_t s_shown_clip;
static uint8_t s_frame_idx;
static lv_image_dsc_t s_ui_icon_dsc[PET_UI_ICON_COUNT];
static uint16_t *s_ui_icon_pix[PET_UI_ICON_COUNT];
static bool s_ui_icon_ok[PET_UI_ICON_COUNT];
static uint32_t s_frame_acc_ms;
static pet_face_id_t s_shown_face;
static uint8_t s_tick_div;
#if PET_VIEW_ENABLE_FACE
static uint32_t s_blink_acc;
static bool s_blinking;
#endif
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

#if PET_VIEW_ENABLE_FACE
static void place_face_part(lv_obj_t *obj, int32_t cx, int32_t cy, int16_t angle_deg,
                            int32_t w, int32_t h)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, cx - (w / 2), cy - (h / 2));
    lv_obj_set_style_transform_pivot_x(obj, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(obj, h / 2, 0);
    /* LVGL transform angle unit: 0.1 degree; clockwise positive matches pack.json. */
    lv_obj_set_style_transform_rotation(obj, (int32_t)angle_deg * 10, 0);
}

static void apply_face_placement(void)
{
    pet_res_face_t face;
    int32_t body_x;
    int32_t body_y;
    int32_t eye_h = 22;
    int32_t mouth_w = 28;
    int32_t mouth_h = 8;
    bool have_face;

    if (s_body_img != NULL && !lv_obj_has_flag(s_body_img, LV_OBJ_FLAG_HIDDEN)) {
        body_x = lv_obj_get_x(s_body_img);
        body_y = lv_obj_get_y(s_body_img);
    } else if (s_body_fallback != NULL) {
        body_x = lv_obj_get_x(s_body_fallback);
        body_y = lv_obj_get_y(s_body_fallback);
    } else {
        body_x = (240 - (int32_t)s_frame_w) / 2;
        body_y = (240 - (int32_t)s_frame_h) / 2 - 6;
    }

    have_face = pet_res_get_face(s_shown_clip, s_frame_idx, &face);
    if (s_shown_face == PET_FACE_SLEEPY) {
        eye_h = 6;
    } else if (s_blinking) {
        eye_h = 3;
    }
    if (s_shown_face == PET_FACE_HAPPY) {
        mouth_w = 36;
        mouth_h = 12;
    } else if (s_shown_face == PET_FACE_SAD) {
        mouth_w = 22;
        mouth_h = 5;
    } else if (s_shown_face == PET_FACE_SLEEPY) {
        mouth_w = 18;
        mouth_h = 4;
    } else if (s_shown_face == PET_FACE_HUNGRY) {
        mouth_w = 16;
        mouth_h = 14;
    } else if (s_shown_face == PET_FACE_ANGRY) {
        mouth_w = 20;
        mouth_h = 6;
    }

    if (have_face) {
        place_face_part(s_eye_l, body_x + face.eye_l.x, body_y + face.eye_l.y,
                        face.eye_l.angle_deg, 18, eye_h);
        place_face_part(s_eye_r, body_x + face.eye_r.x, body_y + face.eye_r.y,
                        face.eye_r.angle_deg, 18, eye_h);
        place_face_part(s_mouth, body_x + face.mouth.x, body_y + face.mouth.y,
                        face.mouth.angle_deg, mouth_w, mouth_h);
        place_face_part(s_brow_l, body_x + face.brow_l.x, body_y + face.brow_l.y,
                        face.brow_l.angle_deg, 16, 3);
        place_face_part(s_brow_r, body_x + face.brow_r.x, body_y + face.brow_r.y,
                        face.brow_r.angle_deg, 16, 3);
        return;
    }

    /* Legacy pack v1: screen-center face (body aligned CENTER,0,-6). */
    place_face_part(s_eye_l, 120 - 18, 120 - 6 - 8, 0, 18, eye_h);
    place_face_part(s_eye_r, 120 + 18, 120 - 6 - 8, 0, 18, eye_h);
    place_face_part(s_mouth, 120, 120 - 6 + 28, 0, mouth_w, mouth_h);
    place_face_part(s_brow_l, 120 - 18, 120 - 6 - 24, 0, 16, 3);
    place_face_part(s_brow_r, 120 + 18, 120 - 6 - 24, 0, 16, 3);
}
#endif /* PET_VIEW_ENABLE_FACE */

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
    s_shown_face = face;
#if !PET_VIEW_ENABLE_FACE
    (void)s_shown_face;
#endif
#if PET_VIEW_ENABLE_FACE
    int32_t eye_h = 22;
    int32_t pupil_y = 0;
    int32_t mouth_w = 28;
    int32_t mouth_h = 8;
    lv_color_t eye_c = lv_color_hex(0xFFFFFF);
    lv_color_t mouth_c = lv_color_hex(0xE07080);
    bool brows = false;

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
        lv_obj_set_style_bg_color(s_mouth, mouth_c, 0);
        lv_obj_set_style_radius(s_mouth, mouth_h / 2 + 2, 0);
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
    apply_face_placement();
#endif
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
#if PET_VIEW_ENABLE_FACE
        apply_face_placement();
#endif
        return;
    }
    if (!load_frame_to(0, clip, 0U)) {
        return;
    }
    s_pix_i = 0U;
    lv_image_set_src(s_body_img, &s_img_dsc[0]);
    lv_obj_set_size(s_body_img, (int32_t)s_frame_w, (int32_t)s_frame_h);
    lv_obj_align(s_body_img, LV_ALIGN_CENTER, 0, -6);
    lv_obj_remove_flag(s_body_img, LV_OBJ_FLAG_HIDDEN);
    if (s_body_fallback != NULL) {
        lv_obj_add_flag(s_body_fallback, LV_OBJ_FLAG_HIDDEN);
    }
#if PET_VIEW_ENABLE_FACE
    apply_face_placement();
#endif
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
#if PET_VIEW_ENABLE_FACE
    apply_face_placement();
#endif
}

#if PET_VIEW_ENABLE_FACE
static void apply_blink(bool closed)
{
    if ((s_shown_face == PET_FACE_SLEEPY) || (s_eye_l == NULL)) {
        return;
    }
    s_blinking = closed;
    apply_face_placement();
}
#endif

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

static const lv_font_t *chat_caption_fallback_font(void)
{
#if LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    return &lv_font_source_han_sans_sc_14_cjk;
#elif LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}

static void chat_font_try_load_sd(void)
{
    char abs[PET_FS_PATH_MAX];
    char lvpath[PET_FS_PATH_MAX + 4U];
    long sz;
    lv_font_t *font;
    const lv_font_t *fallback;

    if (s_sd_caption_font != NULL) {
        return;
    }
#if !LV_USE_FS_STDIO
    (void)abs;
    (void)lvpath;
    (void)sz;
    (void)font;
    (void)fallback;
    return;
#else
    sz = pet_fs_file_size(PET_RES_FONT_CAPTION_REL);
    if (sz <= 0) {
        LOG_INFO("chat font: no %s (use embedded CJK)", PET_RES_FONT_CAPTION_REL);
        return;
    }
    if (!pet_fs_join(abs, sizeof(abs), PET_RES_FONT_CAPTION_REL)) {
        return;
    }
    /* LVGL FS_STDIO letter S → fopen(abs) */
    if (snprintf(lvpath, sizeof(lvpath), "S:%s", abs) < 0) {
        return;
    }
    font = lv_binfont_create(lvpath);
    if (font == NULL) {
        LOG_WARN("chat font: binfont load fail %s (%ld B)", abs, sz);
        return;
    }
    fallback = chat_caption_fallback_font();
    if (fallback != NULL) {
        font->fallback = fallback;
    }
    s_sd_caption_font = font;
    LOG_INFO("chat font: SD %s (%ld B)", PET_RES_FONT_CAPTION_REL, sz);
#endif
}

static const lv_font_t *chat_caption_font(void)
{
    if (s_sd_caption_font != NULL) {
        return s_sd_caption_font;
    }
    return chat_caption_fallback_font();
}

/** Truncate on UTF-8 codepoint boundary (avoid broken CJK glyphs). */
static void utf8_copy_trunc(char *dst, size_t dst_sz, const char *src)
{
    size_t i = 0U;

    if ((dst == NULL) || (dst_sz == 0U)) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    while ((src[i] != '\0') && ((i + 1U) < dst_sz)) {
        unsigned char c = (unsigned char)src[i];
        size_t need = 1U;

        if ((c & 0x80U) == 0U) {
            need = 1U;
        } else if ((c & 0xE0U) == 0xC0U) {
            need = 2U;
        } else if ((c & 0xF0U) == 0xE0U) {
            need = 3U;
        } else if ((c & 0xF8U) == 0xF0U) {
            need = 4U;
        } else {
            break;
        }
        if ((i + need) >= dst_sz) {
            break;
        }
        (void)memcpy(&dst[i], &src[i], need);
        i += need;
    }
    dst[i] = '\0';
}

static void chat_bump_idle(void)
{
    s_chat_idle_ms = 0U;
}

static void chat_set_home_chrome_visible(bool visible)
{
    if (visible) {
        if (s_need_h != NULL) {
            lv_obj_remove_flag(s_need_h, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_need_m != NULL) {
            lv_obj_remove_flag(s_need_m, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_need_e != NULL) {
            lv_obj_remove_flag(s_need_e, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_care_f != NULL) {
            lv_obj_remove_flag(s_care_f, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_care_p != NULL) {
            lv_obj_remove_flag(s_care_p, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_care_s != NULL) {
            lv_obj_remove_flag(s_care_s, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_chat_btn != NULL) {
            lv_obj_remove_flag(s_chat_btn, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (s_need_h != NULL) {
            lv_obj_add_flag(s_need_h, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_need_m != NULL) {
            lv_obj_add_flag(s_need_m, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_need_e != NULL) {
            lv_obj_add_flag(s_need_e, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_care_f != NULL) {
            lv_obj_add_flag(s_care_f, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_care_p != NULL) {
            lv_obj_add_flag(s_care_p, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_care_s != NULL) {
            lv_obj_add_flag(s_care_s, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_chat_btn != NULL) {
            lv_obj_add_flag(s_chat_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_pack_hint != NULL) {
            lv_obj_add_flag(s_pack_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void chat_apply_wave_visible(bool on)
{
    int i;

    for (i = 0; i < PET_CHAT_WAVE_BARS; i++) {
        if (s_chat_wave[i] == NULL) {
            continue;
        }
        if (on) {
            lv_obj_remove_flag(s_chat_wave[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_chat_wave[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void chat_apply_mode_ui(pet_chat_mode_t mode)
{
    const char *label = "ready";
    uint32_t color = 0x7A849CU;

    s_chat_mode_id = mode;
    switch (mode) {
    case PET_CHAT_MODE_CONNECTING:
        label = "connecting";
        color = 0xE0B060U;
        break;
    case PET_CHAT_MODE_LISTENING:
        label = "listen";
        color = 0x7EC8FFU;
        break;
    case PET_CHAT_MODE_SPEAKING:
        label = "speak";
        color = 0x6DD6A0U;
        break;
    case PET_CHAT_MODE_IDLE:
    default:
        label = "ready";
        color = 0x7A849CU;
        break;
    }
    if (s_chat_mode != NULL) {
        lv_label_set_text(s_chat_mode, label);
        lv_obj_set_style_text_color(s_chat_mode, lv_color_hex(color), 0);
    }
    chat_apply_wave_visible(mode == PET_CHAT_MODE_LISTENING);
}

static void chat_set_caption_internal(const char *utf8)
{
    const char *show = "...";

    if ((utf8 != NULL) && (utf8[0] != '\0')) {
        utf8_copy_trunc(s_chat_caption_buf, sizeof(s_chat_caption_buf), utf8);
        show = s_chat_caption_buf;
    } else {
        s_chat_caption_buf[0] = '\0';
    }
    if (s_chat_caption != NULL) {
        lv_label_set_text(s_chat_caption, show);
        if (show[0] == '.' && show[1] == '.' && show[2] == '.' && show[3] == '\0') {
            lv_obj_set_style_text_color(s_chat_caption, lv_color_hex(0x6A7388), 0);
        } else {
            lv_obj_set_style_text_color(s_chat_caption, lv_color_hex(0xDCE6F8), 0);
        }
    }
}

static void chat_close_internal(bool notify)
{
    if (!s_chat_open) {
        return;
    }
    s_chat_open = false;
    s_chat_listen_on = false;
    s_chat_listen_ui_pending = false;
    s_chat_idle_ms = 0U;
    if (s_chat_layer != NULL) {
        lv_obj_add_flag(s_chat_layer, LV_OBJ_FLAG_HIDDEN);
    }
    chat_set_home_chrome_visible(true);
    if (s_home_parent != NULL) {
        lv_obj_set_style_bg_color(s_home_parent, lv_color_hex(0x202020), 0);
    }
    chat_apply_mode_ui(PET_CHAT_MODE_IDLE);
    if (notify && (s_chat_hook != NULL)) {
        s_chat_hook(PET_CHAT_ACT_LEAVE);
    }
}

static void chat_open_internal(void)
{
    if (s_chat_open) {
        return;
    }
    if (s_chat_layer == NULL) {
        return;
    }
    s_chat_open = true;
    s_chat_listen_on = false;
    s_chat_listen_ui_pending = false;
    chat_bump_idle();
    chat_set_home_chrome_visible(false);
    if (s_home_parent != NULL) {
        lv_obj_set_style_bg_color(s_home_parent, lv_color_hex(0x141820), 0);
    }
    lv_obj_remove_flag(s_chat_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_chat_layer);
    if (s_body_hit != NULL) {
        lv_obj_move_foreground(s_body_hit);
    }
    chat_set_caption_internal("tap to talk");
    chat_apply_mode_ui(PET_CHAT_MODE_CONNECTING);
    if (s_chat_hook != NULL) {
        s_chat_hook(PET_CHAT_ACT_ENTER);
    }
}

static void chat_toggle_listen(void)
{
    if (!s_chat_open) {
        return;
    }
    chat_bump_idle();
    if (s_chat_listen_on) {
        s_chat_listen_on = false;
        s_chat_listen_ui_pending = false;
        chat_set_caption_internal("tap to talk");
        if (s_chat_hook != NULL) {
            s_chat_hook(PET_CHAT_ACT_LISTEN_OFF);
        }
    } else {
        s_chat_listen_on = true;
        s_chat_listen_ui_pending = true;
        (void)pet_core_post(PET_EVT_LISTEN, 0);
        if (s_chat_hook != NULL) {
            s_chat_hook(PET_CHAT_ACT_LISTEN_ON);
        }
    }
}

static void chat_back_cb(lv_event_t *e)
{
    (void)e;
    chat_close_internal(true);
}

static void care_cb(lv_event_t *e)
{
    pet_evt_id_t id = (pet_evt_id_t)(uintptr_t)lv_event_get_user_data(e);

    if (s_chat_open) {
        return;
    }
    (void)pet_core_post(id, 0);
}

static void chat_cb(lv_event_t *e)
{
    (void)e;
    chat_open_internal();
}

static void body_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (s_chat_open) {
        if (code == LV_EVENT_CLICKED) {
            chat_toggle_listen();
        }
        return;
    }

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

static void chat_tick_wave(void)
{
    static const int16_t k_h[PET_CHAT_WAVE_BARS] = {10, 18, 24, 14, 20};
    int i;

    if (!s_chat_open || (s_chat_mode_id != PET_CHAT_MODE_LISTENING)) {
        return;
    }
    s_chat_wave_phase++;
    for (i = 0; i < PET_CHAT_WAVE_BARS; i++) {
        int16_t h;
        int16_t phase;

        if (s_chat_wave[i] == NULL) {
            continue;
        }
        phase = (int16_t)((s_chat_wave_phase + (uint8_t)(i * 3U)) & 7U);
        h = (int16_t)(k_h[i] - (phase < 4 ? phase : (8 - phase)));
        if (h < 6) {
            h = 6;
        }
        lv_obj_set_height(s_chat_wave[i], h);
    }
}

static void chat_create_layer(lv_obj_t *parent)
{
    lv_obj_t *row;
    int i;
    static const int16_t k_h[PET_CHAT_WAVE_BARS] = {10, 18, 24, 14, 20};

    s_chat_layer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_chat_layer);
    lv_obj_set_size(s_chat_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_chat_layer, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_chat_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_chat_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_chat_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_chat_back = lv_button_create(s_chat_layer);
    lv_obj_remove_style_all(s_chat_back);
    lv_obj_set_size(s_chat_back, PET_DOCK_BTN_SIZE, PET_DOCK_BTN_SIZE);
    lv_obj_set_style_radius(s_chat_back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_chat_back, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(s_chat_back, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_chat_back, LV_ALIGN_TOP_LEFT, 16, 16);
    lv_obj_set_ext_click_area(s_chat_back, PET_DOCK_BTN_EXT_CLICK);
    lv_obj_add_event_cb(s_chat_back, chat_back_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lbl = lv_label_create(s_chat_back);

        lv_label_set_text(lbl, "<");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xE8EAF0), 0);
#if LV_FONT_MONTSERRAT_14
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
#endif
        lv_obj_center(lbl);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    s_chat_mode = lv_label_create(s_chat_layer);
    lv_label_set_text(s_chat_mode, "listen");
    lv_obj_set_style_text_color(s_chat_mode, lv_color_hex(0x7EC8FF), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(s_chat_mode, &lv_font_montserrat_14, 0);
#endif
    lv_obj_align(s_chat_mode, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_remove_flag(s_chat_mode, LV_OBJ_FLAG_CLICKABLE);

    s_chat_cap_box = lv_obj_create(s_chat_layer);
    lv_obj_remove_style_all(s_chat_cap_box);
    lv_obj_set_size(s_chat_cap_box, PET_CHAT_CAP_BOX_W, PET_CHAT_CAP_BOX_H);
    lv_obj_set_style_radius(s_chat_cap_box, 12, 0);
    lv_obj_set_style_bg_opa(s_chat_cap_box, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_chat_cap_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_hor(s_chat_cap_box, 8, 0);
    lv_obj_set_style_pad_ver(s_chat_cap_box, 6, 0);
    lv_obj_align(s_chat_cap_box, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_remove_flag(s_chat_cap_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_chat_cap_box, LV_OBJ_FLAG_CLICKABLE);

    s_chat_caption = lv_label_create(s_chat_cap_box);
    lv_label_set_long_mode(s_chat_caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_chat_caption, PET_CHAT_CAP_LABEL_W);
    lv_obj_set_style_text_align(s_chat_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_chat_caption, lv_color_hex(0xDCE6F8), 0);
    lv_obj_set_style_text_font(s_chat_caption, chat_caption_font(), 0);
    lv_label_set_text(s_chat_caption, "...");
    lv_obj_center(s_chat_caption);
    lv_obj_remove_flag(s_chat_caption, LV_OBJ_FLAG_CLICKABLE);

    row = lv_obj_create(s_chat_layer);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 40, 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 3, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < PET_CHAT_WAVE_BARS; i++) {
        s_chat_wave[i] = lv_obj_create(row);
        lv_obj_remove_style_all(s_chat_wave[i]);
        lv_obj_set_size(s_chat_wave[i], 4, k_h[i]);
        lv_obj_set_style_radius(s_chat_wave[i], 2, 0);
        lv_obj_set_style_bg_opa(s_chat_wave[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_chat_wave[i], lv_color_hex(0x7EC8FF), 0);
        lv_obj_remove_flag(s_chat_wave[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_chat_wave[i], LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    return dot;
}

static void apply_btn_icon(lv_obj_t *btn, lv_obj_t *lbl, pet_ui_icon_id_t id)
{
    uint16_t w = 0;
    uint16_t h = 0;
    lv_obj_t *img;

    if ((btn == NULL) || (lbl == NULL) || (id >= PET_UI_ICON_COUNT)) {
        return;
    }
    if (!s_ui_icon_ok[id]) {
        if (s_ui_icon_pix[id] == NULL) {
            s_ui_icon_pix[id] = (uint16_t *)view_alloc(PET_UI_ICON_PIXELS * 2U);
        }
        if (s_ui_icon_pix[id] == NULL) {
            return;
        }
        if (!pet_res_load_ui_icon(id, s_ui_icon_pix[id], PET_UI_ICON_PIXELS, &w, &h)) {
            return;
        }
        fill_img_dsc(&s_ui_icon_dsc[id], s_ui_icon_pix[id], w, h);
        s_ui_icon_ok[id] = true;
    }

    img = lv_image_create(btn);
    lv_image_set_src(img, &s_ui_icon_dsc[id]);
    lv_obj_set_size(img, (int32_t)s_ui_icon_dsc[id].header.w, (int32_t)s_ui_icon_dsc[id].header.h);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_care_btn(lv_obj_t *parent, const char *title, int32_t x, int32_t y,
                               pet_evt_id_t evt, pet_ui_icon_id_t icon)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl;

    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, PET_DOCK_BTN_SIZE, PET_DOCK_BTN_SIZE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A44), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    lv_obj_set_ext_click_area(btn, PET_DOCK_BTN_EXT_CLICK);
    lv_obj_add_event_cb(btn, care_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)evt);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xE8EAF0), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
#endif
    lv_obj_center(lbl);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    apply_btn_icon(btn, lbl, icon);
    return btn;
}

/* Chat 独立到圆屏右侧；有 theme/ui/chat.bin 则用图标，否则字母 C */
static lv_obj_t *make_chat_btn(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lbl;

    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, PET_DOCK_BTN_SIZE, PET_DOCK_BTN_SIZE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A5570), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x7EC8FF), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_70, 0);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_ext_click_area(btn, PET_DOCK_BTN_EXT_CLICK);
    lv_obj_add_event_cb(btn, chat_cb, LV_EVENT_CLICKED, NULL);
    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "C");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDCEEFF), 0);
#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
#endif
    lv_obj_center(lbl);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    apply_btn_icon(btn, lbl, PET_UI_ICON_CHAT);
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

#if PET_VIEW_ENABLE_FACE
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
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_CLICKABLE);

    pupil = lv_obj_create(eye);
    lv_obj_remove_style_all(pupil);
    lv_obj_set_size(pupil, 8, 10);
    lv_obj_set_style_bg_color(pupil, lv_color_hex(0x202028), 0);
    lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(pupil);
    lv_obj_remove_flag(pupil, LV_OBJ_FLAG_CLICKABLE);
    if (x < 0) {
        s_pupil_l = pupil;
    } else {
        s_pupil_r = pupil;
    }
    return eye;
}
#endif /* PET_VIEW_ENABLE_FACE */

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

    /* Gate 已满足（≥1s + pack）：此时再拉字库，避免卡死首帧刷新导致“无开机动画”。 */
    chat_font_try_load_sd();
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

void pet_view_set_chat_hook(pet_view_chat_hook_t hook)
{
    s_chat_hook = hook;
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
    lv_obj_remove_flag(s_body_fallback, LV_OBJ_FLAG_CLICKABLE);

    s_body_img = lv_image_create(parent);
    lv_obj_align(s_body_img, LV_ALIGN_CENTER, 0, -6);
    lv_obj_add_flag(s_body_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_body_img, LV_OBJ_FLAG_CLICKABLE);

#if PET_VIEW_ENABLE_FACE
    s_eye_l = make_eye(parent, -18);
    s_eye_r = make_eye(parent, 18);

    s_brow_l = lv_obj_create(parent);
    lv_obj_remove_style_all(s_brow_l);
    lv_obj_set_size(s_brow_l, 16, 3);
    lv_obj_set_style_bg_color(s_brow_l, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(s_brow_l, LV_OPA_COVER, 0);
    lv_obj_align(s_brow_l, LV_ALIGN_CENTER, -18, -24);
    lv_obj_add_flag(s_brow_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_brow_l, LV_OBJ_FLAG_CLICKABLE);

    s_brow_r = lv_obj_create(parent);
    lv_obj_remove_style_all(s_brow_r);
    lv_obj_set_size(s_brow_r, 16, 3);
    lv_obj_set_style_bg_color(s_brow_r, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(s_brow_r, LV_OPA_COVER, 0);
    lv_obj_align(s_brow_r, LV_ALIGN_CENTER, 18, -24);
    lv_obj_add_flag(s_brow_r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_brow_r, LV_OBJ_FLAG_CLICKABLE);

    s_mouth = lv_obj_create(parent);
    lv_obj_remove_style_all(s_mouth);
    lv_obj_set_size(s_mouth, 28, 8);
    lv_obj_set_style_bg_color(s_mouth, lv_color_hex(0xE07080), 0);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mouth, 6, 0);
    lv_obj_align(s_mouth, LV_ALIGN_CENTER, 0, 28);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_CLICKABLE);
#endif

    hit = lv_obj_create(parent);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, PET_BODY_HIT_W, PET_BODY_HIT_H);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_align(hit, LV_ALIGN_CENTER, 0, PET_BODY_HIT_Y);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, body_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hit, body_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(hit, body_event_cb, LV_EVENT_CLICKED, NULL);
    s_body_hit = hit;

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

    /* 左侧护理弧 F/P/S（独立、间距加大）；右侧 Chat */
    s_care_f = make_care_btn(parent, "F", PET_CARE_ARC_X_WING, -PET_CARE_ARC_Y, PET_EVT_CARE_FEED,
                             PET_UI_ICON_FEED);
    s_care_p = make_care_btn(parent, "P", PET_CARE_ARC_X_MID, 0, PET_EVT_CARE_PLAY, PET_UI_ICON_PLAY);
    s_care_s = make_care_btn(parent, "S", PET_CARE_ARC_X_WING, PET_CARE_ARC_Y, PET_EVT_CARE_SLEEP,
                             PET_UI_ICON_SLEEP);
    s_chat_btn = make_chat_btn(parent);
    lv_obj_move_foreground(s_care_f);
    lv_obj_move_foreground(s_care_p);
    lv_obj_move_foreground(s_care_s);
    lv_obj_move_foreground(s_chat_btn);

    s_home_parent = parent;
    chat_create_layer(parent);

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

    if (s_chat_open) {
        if (s_chat_listen_ui_pending) {
            s_chat_listen_ui_pending = false;
            if (s_chat_listen_on) {
                chat_apply_mode_ui(PET_CHAT_MODE_LISTENING);
                chat_set_caption_internal(NULL);
            }
        }
        s_chat_idle_ms += PET_VIEW_POLL_MS;
        chat_tick_wave();
        if (s_chat_idle_ms >= PET_CHAT_IDLE_MS) {
            chat_close_internal(true);
        }
    } else if (s_pressing) {
        s_press_ms += PET_VIEW_POLL_MS;
        if (!s_hold_sent && (s_press_ms >= PET_VIEW_HOLD_MS)) {
            s_hold_sent = true;
            (void)pet_core_post(PET_EVT_TOUCH_HOLD, 0);
        }
    }

#if PET_VIEW_ENABLE_FACE
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
#endif
}

bool pet_view_chat_is_open(void)
{
    return s_chat_open;
}

void pet_view_chat_close(void)
{
    chat_close_internal(true);
}

void pet_view_chat_set_mode(pet_chat_mode_t mode)
{
    if (!s_chat_open) {
        return;
    }
    if (mode == s_chat_mode_id) {
        return;
    }
    chat_apply_mode_ui(mode);
    if ((mode == PET_CHAT_MODE_LISTENING) || (mode == PET_CHAT_MODE_SPEAKING) ||
        (mode == PET_CHAT_MODE_CONNECTING)) {
        chat_bump_idle();
    }
    if (mode == PET_CHAT_MODE_SPEAKING) {
        /* 进入说态后本地听开关复位，下一击可打断再听。 */
        s_chat_listen_on = false;
        s_chat_listen_ui_pending = false;
        (void)pet_core_post(PET_EVT_SPEAK, 0);
    } else if (mode == PET_CHAT_MODE_IDLE) {
        s_chat_listen_on = false;
        s_chat_listen_ui_pending = false;
    } else if (mode == PET_CHAT_MODE_LISTENING) {
        (void)pet_core_post(PET_EVT_LISTEN, 0);
    }
}

void pet_view_chat_set_caption(const char *utf8)
{
    chat_set_caption_internal(utf8);
    if (s_chat_open && (utf8 != NULL) && (utf8[0] != '\0')) {
        chat_bump_idle();
    }
}

void pet_view_chat_bump_idle(void)
{
    if (s_chat_open) {
        chat_bump_idle();
    }
}
