/**
 * @file pet_view.h
 * @brief LVGL layered pet: SD body + drawn face + needs HUD.
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

void pet_view_set_alloc(pet_view_alloc_fn alloc_fn, pet_view_free_fn free_fn);
void pet_view_set_intent_hook(pet_view_intent_hook_t hook);
void pet_view_create(lv_obj_t *parent);
void pet_view_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* PET_VIEW_H */
