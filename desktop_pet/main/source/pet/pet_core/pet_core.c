/**
 * @file pet_core.c
 * @brief Needs decay + clip priority FSM. Single-threaded (LVGL task).
 */

#include "pet_core.h"

#include <stddef.h>

#define PET_Q_LEN (16U)
#define PET_NEED_MAX (100U)
#define PET_NEEDS_PERIOD_S (30U) /* consume / HUD persist cadence */
#define PET_HUNGRY_TH (25U)
#define PET_SAD_TH (25U)
#define PET_SLEEPY_TH (25U)
#define PET_PLAY_ENERGY_COST (8U)
#define PET_FEED_FULL_TH (85U) /* hunger >= → refuse, no hunger gain */
#define PET_POKE_COLD_TH (30U) /* mood < → refuse, no mood gain */

typedef struct {
    pet_evt_id_t id;
    int16_t arg0;
} pet_evt_t;

static const pet_needs_cfg_t s_default_cfg = {
    /* Desktop-friendly: ~1% per many minutes; applied in 30s steps. */
    .hunger_decay_s = 900U,   /* 15 min / % */
    .mood_decay_s = 1200U,    /* 20 min / % */
    .energy_decay_s = 1500U,  /* 25 min / % */
    .energy_recover_s = 180U, /* sleep: +1% / 3 min */
    .feed_hunger = 28U,
    .play_mood = 24U,
    .tap_mood = 4U,
};

static const uint8_t s_clip_prio[PET_CLIP_COUNT] = {
    [PET_CLIP_IDLE] = 0U,
    [PET_CLIP_SLEEPY] = 0U,
    [PET_CLIP_SAD] = 0U,
    [PET_CLIP_SLEEP_LOOP] = 0U,
    [PET_CLIP_POKE] = 10U,
    [PET_CLIP_REFUSE] = 15U,
    [PET_CLIP_EAT] = 20U,
    [PET_CLIP_PLAY] = 20U,
};

static const uint8_t s_clip_dur_s[PET_CLIP_COUNT] = {
    [PET_CLIP_IDLE] = 0U,
    [PET_CLIP_SLEEPY] = 0U,
    [PET_CLIP_SAD] = 0U,
    [PET_CLIP_SLEEP_LOOP] = 0U,
    [PET_CLIP_POKE] = 1U,
    [PET_CLIP_REFUSE] = 2U,
    [PET_CLIP_EAT] = 3U,
    [PET_CLIP_PLAY] = 4U,
};

static pet_needs_cfg_t s_cfg;
static pet_needs_t s_needs;
static pet_clip_id_t s_clip;
static pet_face_id_t s_face;
static uint8_t s_clip_remain_s;
static uint16_t s_hunger_acc;
static uint16_t s_mood_acc;
static uint16_t s_energy_acc;
static uint8_t s_needs_period_s; /* counts to PET_NEEDS_PERIOD_S */

static pet_evt_t s_evt_q[PET_Q_LEN];
static uint8_t s_evt_head;
static uint8_t s_evt_tail;
static uint8_t s_evt_count;

static pet_intent_t s_int_q[PET_Q_LEN];
static uint8_t s_int_head;
static uint8_t s_int_tail;
static uint8_t s_int_count;

static uint8_t sat_add_u8(uint8_t v, uint8_t d)
{
    uint16_t s = (uint16_t)v + (uint16_t)d;

    if (s > PET_NEED_MAX) {
        s = PET_NEED_MAX;
    }
    return (uint8_t)s;
}

static uint8_t sat_sub_u8(uint8_t v, uint8_t d)
{
    if (v <= d) {
        return 0U;
    }
    return (uint8_t)(v - d);
}

static void intent_push(pet_intent_id_t id, int16_t a0, int16_t a1)
{
    if (s_int_count >= PET_Q_LEN) {
        s_int_tail = (uint8_t)((s_int_tail + 1U) % PET_Q_LEN);
        s_int_count--;
    }
    s_int_q[s_int_head].id = id;
    s_int_q[s_int_head].arg0 = a0;
    s_int_q[s_int_head].arg1 = a1;
    s_int_head = (uint8_t)((s_int_head + 1U) % PET_Q_LEN);
    s_int_count++;
}

static void emit_hud(void)
{
    intent_push(PET_INTENT_HUD, 0, 0);
}

static void set_face(pet_face_id_t face)
{
    if (face >= PET_FACE_COUNT) {
        face = PET_FACE_IDLE;
    }
    if (s_face != face) {
        s_face = face;
        intent_push(PET_INTENT_FACE, (int16_t)face, 0);
    }
}

static bool start_clip(pet_clip_id_t clip)
{
    uint8_t prio;

    if (clip >= PET_CLIP_COUNT) {
        return false;
    }
    prio = s_clip_prio[clip];
    if ((s_clip_remain_s > 0U) && (prio <= s_clip_prio[s_clip])) {
        return false;
    }
    s_clip = clip;
    s_clip_remain_s = s_clip_dur_s[clip];
    intent_push(PET_INTENT_CLIP, (int16_t)clip, 0);
    if (clip == PET_CLIP_EAT) {
        intent_push(PET_INTENT_LED, 0, 0);
        intent_push(PET_INTENT_MOTOR, 0, 0);
        intent_push(PET_INTENT_SFX, 0, 0);
        set_face(PET_FACE_HAPPY);
    } else if (clip == PET_CLIP_PLAY) {
        intent_push(PET_INTENT_LED, 1, 0);
        intent_push(PET_INTENT_MOTOR, 1, 0);
        intent_push(PET_INTENT_SFX, 1, 0);
        set_face(PET_FACE_HAPPY);
    } else if (clip == PET_CLIP_POKE) {
        set_face(PET_FACE_HAPPY);
    } else if (clip == PET_CLIP_REFUSE) {
        set_face(PET_FACE_SAD);
    }
    return true;
}

static void pick_idle_clip(void)
{
    if (s_needs.sleeping) {
        if (s_clip != PET_CLIP_SLEEP_LOOP) {
            s_clip = PET_CLIP_SLEEP_LOOP;
            s_clip_remain_s = 0U;
            intent_push(PET_INTENT_CLIP, (int16_t)PET_CLIP_SLEEP_LOOP, 0);
        }
        set_face(PET_FACE_SLEEPY);
        return;
    }
    if (s_needs.energy < PET_SLEEPY_TH) {
        if (s_clip != PET_CLIP_SLEEPY) {
            s_clip = PET_CLIP_SLEEPY;
            s_clip_remain_s = 0U;
            intent_push(PET_INTENT_CLIP, (int16_t)PET_CLIP_SLEEPY, 0);
        }
        set_face(PET_FACE_SLEEPY);
        return;
    }
    if (s_needs.hunger < PET_HUNGRY_TH) {
        if (s_clip != PET_CLIP_IDLE) {
            s_clip = PET_CLIP_IDLE;
            s_clip_remain_s = 0U;
            intent_push(PET_INTENT_CLIP, (int16_t)PET_CLIP_IDLE, 0);
        }
        set_face(PET_FACE_HUNGRY);
        return;
    }
    if (s_needs.mood < PET_SAD_TH) {
        if (s_clip != PET_CLIP_SAD) {
            s_clip = PET_CLIP_SAD;
            s_clip_remain_s = 0U;
            intent_push(PET_INTENT_CLIP, (int16_t)PET_CLIP_SAD, 0);
        }
        set_face(PET_FACE_SAD);
        return;
    }
    if (s_clip != PET_CLIP_IDLE) {
        s_clip = PET_CLIP_IDLE;
        s_clip_remain_s = 0U;
        intent_push(PET_INTENT_CLIP, (int16_t)PET_CLIP_IDLE, 0);
    }
    set_face(PET_FACE_IDLE);
}

static bool decay_one(uint8_t *need, uint16_t *acc, uint16_t period_s, uint16_t step_s)
{
    bool changed = false;

    if ((period_s == 0U) || (need == NULL) || (acc == NULL) || (step_s == 0U)) {
        return false;
    }
    *acc = (uint16_t)(*acc + step_s);
    while (*acc >= period_s) {
        *acc = (uint16_t)(*acc - period_s);
        *need = sat_sub_u8(*need, 1U);
        changed = true;
    }
    return changed;
}

static bool recover_one(uint8_t *need, uint16_t *acc, uint16_t period_s, uint16_t step_s)
{
    bool changed = false;

    if ((period_s == 0U) || (need == NULL) || (acc == NULL) || (step_s == 0U)) {
        return false;
    }
    *acc = (uint16_t)(*acc + step_s);
    while (*acc >= period_s) {
        *acc = (uint16_t)(*acc - period_s);
        *need = sat_add_u8(*need, 1U);
        changed = true;
    }
    return changed;
}

static void handle_needs_period(void)
{
    bool changed = false;

    if (s_needs.sleeping) {
        if (decay_one(&s_needs.hunger, &s_hunger_acc, s_cfg.hunger_decay_s, PET_NEEDS_PERIOD_S)) {
            changed = true;
        }
        s_mood_acc = 0U;
        if (recover_one(&s_needs.energy, &s_energy_acc, s_cfg.energy_recover_s, PET_NEEDS_PERIOD_S)) {
            changed = true;
        }
    } else {
        if (decay_one(&s_needs.hunger, &s_hunger_acc, s_cfg.hunger_decay_s, PET_NEEDS_PERIOD_S)) {
            changed = true;
        }
        if (decay_one(&s_needs.mood, &s_mood_acc, s_cfg.mood_decay_s, PET_NEEDS_PERIOD_S)) {
            changed = true;
        }
        if (decay_one(&s_needs.energy, &s_energy_acc, s_cfg.energy_decay_s, PET_NEEDS_PERIOD_S)) {
            changed = true;
        }
    }
    if (changed) {
        emit_hud();
    }
}

static void handle_tick(void)
{
    s_needs_period_s++;
    if (s_needs_period_s >= PET_NEEDS_PERIOD_S) {
        s_needs_period_s = 0U;
        handle_needs_period();
    }

    if (s_clip_remain_s > 0U) {
        s_clip_remain_s--;
        if (s_clip_remain_s == 0U) {
            pick_idle_clip();
        }
    } else {
        pick_idle_clip();
    }
}

static void do_wake(void)
{
    if (!s_needs.sleeping) {
        return;
    }
    s_needs.sleeping = false;
    s_energy_acc = 0U;
    emit_hud();
    pick_idle_clip();
}

static void do_sleep(void)
{
    s_needs.sleeping = true;
    s_clip_remain_s = 0U;
    emit_hud();
    pick_idle_clip();
}

static void do_feed(void)
{
    do_wake();
    /* Full: hard refuse — no hunger/mood gain. */
    if (s_needs.hunger >= PET_FEED_FULL_TH) {
        (void)start_clip(PET_CLIP_REFUSE);
        return;
    }
    s_needs.hunger = sat_add_u8(s_needs.hunger, s_cfg.feed_hunger);
    s_needs.mood = sat_add_u8(s_needs.mood, (uint8_t)(s_cfg.tap_mood + 2U));
    emit_hud();
    /* yum (<40) vs ok share eat until pack variants exist. */
    (void)start_clip(PET_CLIP_EAT);
}

static void do_play(void)
{
    do_wake();
    s_needs.mood = sat_add_u8(s_needs.mood, s_cfg.play_mood);
    s_needs.energy = sat_sub_u8(s_needs.energy, PET_PLAY_ENERGY_COST);
    emit_hud();
    (void)start_clip(PET_CLIP_PLAY);
}

static void do_tap(void)
{
    if (s_needs.sleeping) {
        do_wake();
        return;
    }
    /* Cold: visible refuse, no mood gain. */
    if (s_needs.mood < PET_POKE_COLD_TH) {
        (void)start_clip(PET_CLIP_REFUSE);
        return;
    }
    s_needs.mood = sat_add_u8(s_needs.mood, s_cfg.tap_mood);
    emit_hud();
    (void)start_clip(PET_CLIP_POKE);
}

static void handle_evt(const pet_evt_t *e)
{
    if (e == NULL) {
        return;
    }
    switch (e->id) {
    case PET_EVT_TICK_1S:
        handle_tick();
        break;
    case PET_EVT_TOUCH_TAP:
        do_tap();
        break;
    case PET_EVT_TOUCH_HOLD:
        do_feed();
        break;
    case PET_EVT_IMU_SHAKE:
        do_play();
        break;
    case PET_EVT_IMU_FLIP:
        do_sleep();
        break;
    case PET_EVT_CARE_FEED:
        do_feed();
        break;
    case PET_EVT_CARE_PLAY:
        do_play();
        break;
    case PET_EVT_CARE_SLEEP:
        do_sleep();
        break;
    case PET_EVT_CARE_WAKE:
        do_wake();
        break;
    case PET_EVT_LISTEN:
        set_face(PET_FACE_IDLE);
        break;
    case PET_EVT_SPEAK:
        set_face(PET_FACE_HAPPY);
        break;
    case PET_EVT_EMOTION:
        if ((e->arg0 >= 0) && (e->arg0 < (int16_t)PET_FACE_COUNT)) {
            set_face((pet_face_id_t)e->arg0);
        }
        break;
    default:
        break;
    }
}

const pet_needs_cfg_t *pet_core_default_cfg(void)
{
    return &s_default_cfg;
}

void pet_core_set_cfg(const pet_needs_cfg_t *cfg)
{
    if (cfg == NULL) {
        s_cfg = s_default_cfg;
        return;
    }
    s_cfg = *cfg;
    if (s_cfg.hunger_decay_s == 0U) {
        s_cfg.hunger_decay_s = s_default_cfg.hunger_decay_s;
    }
    if (s_cfg.mood_decay_s == 0U) {
        s_cfg.mood_decay_s = s_default_cfg.mood_decay_s;
    }
    if (s_cfg.energy_decay_s == 0U) {
        s_cfg.energy_decay_s = s_default_cfg.energy_decay_s;
    }
    if (s_cfg.energy_recover_s == 0U) {
        s_cfg.energy_recover_s = s_default_cfg.energy_recover_s;
    }
}

void pet_core_init(const pet_needs_cfg_t *cfg)
{
    pet_core_set_cfg(cfg);
    s_needs.hunger = 70U;
    s_needs.mood = 70U;
    s_needs.energy = 80U;
    s_needs.sleeping = false;
    s_clip = PET_CLIP_IDLE;
    s_face = PET_FACE_IDLE;
    s_clip_remain_s = 0U;
    s_hunger_acc = 0U;
    s_mood_acc = 0U;
    s_energy_acc = 0U;
    s_needs_period_s = 0U;
    s_evt_head = 0U;
    s_evt_tail = 0U;
    s_evt_count = 0U;
    s_int_head = 0U;
    s_int_tail = 0U;
    s_int_count = 0U;
    intent_push(PET_INTENT_CLIP, (int16_t)PET_CLIP_IDLE, 0);
    intent_push(PET_INTENT_FACE, (int16_t)PET_FACE_IDLE, 0);
    emit_hud();
}

void pet_core_set_needs(const pet_needs_t *needs)
{
    if (needs == NULL) {
        return;
    }
    s_needs.hunger = (needs->hunger > PET_NEED_MAX) ? PET_NEED_MAX : needs->hunger;
    s_needs.mood = (needs->mood > PET_NEED_MAX) ? PET_NEED_MAX : needs->mood;
    s_needs.energy = (needs->energy > PET_NEED_MAX) ? PET_NEED_MAX : needs->energy;
    s_needs.sleeping = needs->sleeping;
    s_hunger_acc = 0U;
    s_mood_acc = 0U;
    s_energy_acc = 0U;
    s_needs_period_s = 0U;
    pick_idle_clip();
    emit_hud();
}

bool pet_core_post(pet_evt_id_t id, int16_t arg0)
{
    if (id >= PET_EVT_COUNT) {
        return false;
    }
    if (s_evt_count >= PET_Q_LEN) {
        return false;
    }
    s_evt_q[s_evt_head].id = id;
    s_evt_q[s_evt_head].arg0 = arg0;
    s_evt_head = (uint8_t)((s_evt_head + 1U) % PET_Q_LEN);
    s_evt_count++;
    return true;
}

void pet_core_poll(void)
{
    while (s_evt_count > 0U) {
        pet_evt_t e = s_evt_q[s_evt_tail];

        s_evt_tail = (uint8_t)((s_evt_tail + 1U) % PET_Q_LEN);
        s_evt_count--;
        handle_evt(&e);
    }
}

void pet_core_get_needs(pet_needs_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_needs;
}

pet_clip_id_t pet_core_clip(void)
{
    return s_clip;
}

pet_face_id_t pet_core_face(void)
{
    return s_face;
}

bool pet_core_take_intent(pet_intent_t *out)
{
    if ((out == NULL) || (s_int_count == 0U)) {
        return false;
    }
    *out = s_int_q[s_int_tail];
    s_int_tail = (uint8_t)((s_int_tail + 1U) % PET_Q_LEN);
    s_int_count--;
    return true;
}

const char *pet_clip_name(pet_clip_id_t id)
{
    static const char *const names[PET_CLIP_COUNT] = {
        "idle", "sleepy", "eat", "play", "sad", "sleep_loop", "poke", "refuse",
    };

    if (id >= PET_CLIP_COUNT) {
        return "?";
    }
    return names[id];
}

const char *pet_face_name(pet_face_id_t id)
{
    static const char *const names[PET_FACE_COUNT] = {
        "idle", "happy", "sad", "sleepy", "hungry", "angry",
    };

    if (id >= PET_FACE_COUNT) {
        return "?";
    }
    return names[id];
}
