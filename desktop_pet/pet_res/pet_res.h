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

bool pet_res_load(void);
void pet_res_unload(void);
bool pet_res_is_loaded(void);
const pet_needs_cfg_t *pet_res_needs_cfg(void);
uint8_t pet_res_clip_frame_count(pet_clip_id_t clip);
uint8_t pet_res_clip_fps(pet_clip_id_t clip);
bool pet_res_load_frame(pet_clip_id_t clip, uint8_t frame_index,
                        uint16_t *pixels, uint32_t pixel_cap,
                        uint16_t *w, uint16_t *h);

#ifdef __cplusplus
}
#endif

#endif /* PET_RES_H */
