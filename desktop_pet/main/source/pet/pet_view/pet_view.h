/**
 * @file pet_view.h
 * @brief LVGL layered pet: SD body + Needs + net/settings + care/Chat; chat surface D.
 *        Settings: scrollable device info + touch calib + EN/ZH; skin switch reserved.
 *        Face overlay reserved (PET_VIEW_ENABLE_FACE).
 */

#ifndef PET_VIEW_H
#define PET_VIEW_H

#include "pet_core.h"

#include "lvgl.h"

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*pet_view_alloc_fn)(size_t nbytes);
typedef void (*pet_view_free_fn)(void *p);
typedef void (*pet_view_intent_hook_t)(const pet_intent_t *intent);
/** Called after Splash Gate opens and home UI is created. */
typedef void (*pet_view_boot_done_fn)(lv_obj_t *parent);

typedef enum {
    PET_CHAT_MODE_IDLE = 0,
    PET_CHAT_MODE_CONNECTING,
    PET_CHAT_MODE_LISTENING,
    PET_CHAT_MODE_SPEAKING,
} pet_chat_mode_t;

typedef enum {
    PET_CHAT_ACT_ENTER = 0, /* 进页：只连 WS */
    PET_CHAT_ACT_LEAVE,     /* 离开：关会话 */
    PET_CHAT_ACT_LISTEN_ON, /* 单击 → 开始听（含打断 TTS） */
    PET_CHAT_ACT_LISTEN_OFF, /* 再单击 → 停听等答 */
} pet_chat_act_t;

typedef void (*pet_view_chat_hook_t)(pet_chat_act_t act);

void pet_view_set_alloc(pet_view_alloc_fn alloc_fn, pet_view_free_fn free_fn);
void pet_view_set_intent_hook(pet_view_intent_hook_t hook);
void pet_view_set_chat_hook(pet_view_chat_hook_t hook);
/**
 * Show splash C (body + ring). Call pet_view_boot_pack_done() after pack
 * load attempt; gate ≥1s + pack done, then load caption font and open home.
 * Does not wait for Wi‑Fi / agent.
 */
void pet_view_boot_start(lv_obj_t *parent, pet_view_boot_done_fn on_home);
/** Mark pack load attempt finished (Splash Gate half). */
void pet_view_boot_pack_done(void);
void pet_view_create(lv_obj_t *parent);
void pet_view_poll(void);

bool pet_view_chat_is_open(void);
void pet_view_chat_close(void);
void pet_view_chat_set_mode(pet_chat_mode_t mode);
/** Current-turn caption (STT / assistant). NULL or "" → placeholder. UTF-8. */
void pet_view_chat_set_caption(const char *utf8);
/** Reset 45s idle timer (call on STT / listen activity). */
void pet_view_chat_bump_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* PET_VIEW_H */
