/**
 * @file pet_view.h
 * @brief LVGL layered pet: SD body + face + Needs dots + Dock.
 */

#ifndef PET_VIEW_H
#define PET_VIEW_H

#include "pet_core.h"

#include "lvgl.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*pet_view_alloc_fn)(size_t nbytes);
typedef void (*pet_view_free_fn)(void *p);
typedef void (*pet_view_intent_hook_t)(const pet_intent_t *intent);
/** Called after Splash Gate opens and home UI is created. */
typedef void (*pet_view_boot_done_fn)(lv_obj_t *parent);

void pet_view_set_alloc(pet_view_alloc_fn alloc_fn, pet_view_free_fn free_fn);
void pet_view_set_intent_hook(pet_view_intent_hook_t hook);
/**
 * Show splash C (body + ring). Call pet_view_boot_pack_done() after pack
 * load attempt ends; gate is ≥1s + pack done, then creates home.
 */
void pet_view_boot_start(lv_obj_t *parent, pet_view_boot_done_fn on_home);
void pet_view_boot_pack_done(void);
void pet_view_create(lv_obj_t *parent);
void pet_view_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* PET_VIEW_H */
