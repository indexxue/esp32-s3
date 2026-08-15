/**
 * @file pet_core.h
 * @brief Portable Tamagotchi brain: events, needs, clip FSM. No LVGL / IDF.
 */

#ifndef PET_CORE_H
#define PET_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PET_EVT_TICK_1S = 0,
    PET_EVT_TOUCH_TAP,
    PET_EVT_TOUCH_HOLD,
    PET_EVT_IMU_SHAKE,
    PET_EVT_IMU_FLIP,
    PET_EVT_CARE_FEED,
    PET_EVT_CARE_PLAY,
    PET_EVT_CARE_SLEEP,
    PET_EVT_CARE_WAKE,
    PET_EVT_LISTEN,
    PET_EVT_SPEAK,
    PET_EVT_EMOTION,
    PET_EVT_COUNT
} pet_evt_id_t;

typedef enum {
    PET_CLIP_IDLE = 0,
    PET_CLIP_SLEEPY,
    PET_CLIP_EAT,
    PET_CLIP_PLAY,
    PET_CLIP_SAD,
    PET_CLIP_SLEEP_LOOP,
    PET_CLIP_POKE,
    PET_CLIP_REFUSE, /* one-shot: feed full / cold poke; pack may omit → sad frames */
    PET_CLIP_COUNT
} pet_clip_id_t;

typedef enum {
    PET_FACE_IDLE = 0,
    PET_FACE_HAPPY,
    PET_FACE_SAD,
    PET_FACE_SLEEPY,
    PET_FACE_HUNGRY,
    PET_FACE_ANGRY,
    PET_FACE_COUNT
} pet_face_id_t;

typedef enum {
    PET_INTENT_CLIP = 0,
    PET_INTENT_FACE,
    PET_INTENT_HUD,
    PET_INTENT_LED,
    PET_INTENT_MOTOR,
    PET_INTENT_SFX,
    PET_INTENT_OPEN_CHAT, /* Dock「聊」→ 对话页（view 直投 hook，不经 core） */
    PET_INTENT_COUNT
} pet_intent_id_t;

typedef struct {
    pet_intent_id_t id;
    int16_t arg0;
    int16_t arg1;
} pet_intent_t;

typedef struct {
    uint8_t hunger;
    uint8_t mood;
    uint8_t energy;
    bool sleeping;
} pet_needs_t;

typedef struct {
    uint16_t hunger_decay_s;
    uint16_t mood_decay_s;
    uint16_t energy_decay_s;
    uint16_t energy_recover_s;
    uint8_t feed_hunger;
    uint8_t play_mood;
    uint8_t tap_mood;
} pet_needs_cfg_t;

void pet_core_init(const pet_needs_cfg_t *cfg);
void pet_core_set_cfg(const pet_needs_cfg_t *cfg);
/** Apply persisted percentages (0–100) + sleeping; call after init. */
void pet_core_set_needs(const pet_needs_t *needs);
bool pet_core_post(pet_evt_id_t id, int16_t arg0);
void pet_core_poll(void);
void pet_core_get_needs(pet_needs_t *out);
pet_clip_id_t pet_core_clip(void);
pet_face_id_t pet_core_face(void);
bool pet_core_take_intent(pet_intent_t *out);
const char *pet_clip_name(pet_clip_id_t id);
const char *pet_face_name(pet_face_id_t id);
const pet_needs_cfg_t *pet_core_default_cfg(void);

#ifdef __cplusplus
}
#endif

#endif /* PET_CORE_H */
