/**
 * @file pet_res.h
 * @brief Load pack.bin + RGBH body frames via pet_fs.
 */

#ifndef PET_RES_H
#define PET_RES_H

#include "pet_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PET_RES_MAX_CLIPS (8U)
#define PET_RES_MAX_FRAMES (8U)
#define PET_RES_NAME_LEN (32U)
#define PET_RES_FRAME_MAX_W (180U)
#define PET_RES_FRAME_MAX_H (180U)
#define PET_RES_SPLASH_MAX_W (240U)
#define PET_RES_SPLASH_MAX_H (240U)
#define PET_RES_SPLASH_REL "boot/splash.bin"
#define PET_RES_UI_ICON_MAX_W (32U)
#define PET_RES_UI_ICON_MAX_H (32U)
#define PET_RES_UI_ICON_FEED_REL "theme/ui/feed.bin"
#define PET_RES_UI_ICON_PLAY_REL "theme/ui/play.bin"
#define PET_RES_UI_ICON_SLEEP_REL "theme/ui/sleep.bin"
#define PET_RES_UI_ICON_CHAT_REL "theme/ui/chat.bin"
#define PET_RES_FACE_BLOCK_LEN (32U)

typedef enum {
    PET_UI_ICON_FEED = 0,
    PET_UI_ICON_PLAY,
    PET_UI_ICON_SLEEP,
    PET_UI_ICON_CHAT,
    PET_UI_ICON_COUNT
} pet_ui_icon_id_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t angle_deg;
} pet_res_face_part_t;

typedef struct {
    pet_res_face_part_t eye_l;
    pet_res_face_part_t eye_r;
    pet_res_face_part_t mouth;
    pet_res_face_part_t brow_l;
    pet_res_face_part_t brow_r;
    bool valid; /* false → pet_view uses legacy screen-center placement */
} pet_res_face_t;

bool pet_res_load(void);
void pet_res_unload(void);
bool pet_res_is_loaded(void);
uint16_t pet_res_pack_version(void);
const pet_needs_cfg_t *pet_res_needs_cfg(void);
uint8_t pet_res_clip_frame_count(pet_clip_id_t clip);
uint8_t pet_res_clip_fps(pet_clip_id_t clip);
bool pet_res_load_rgbh(const char *rel, uint16_t *pixels, uint32_t pixel_cap,
                       uint16_t max_w, uint16_t max_h, uint16_t *w, uint16_t *h);
bool pet_res_load_frame(pet_clip_id_t clip, uint8_t frame_index,
                        uint16_t *pixels, uint32_t pixel_cap,
                        uint16_t *w, uint16_t *h);
bool pet_res_get_face(pet_clip_id_t clip, uint8_t frame_index, pet_res_face_t *out);
bool pet_res_load_splash(uint16_t *pixels, uint32_t pixel_cap,
                         uint16_t *w, uint16_t *h);
bool pet_res_load_ui_icon(pet_ui_icon_id_t id, uint16_t *pixels, uint32_t pixel_cap,
                          uint16_t *w, uint16_t *h);

#ifdef __cplusplus
}
#endif

#endif /* PET_RES_H */
