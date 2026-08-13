/**
 * @file pet_core_smoke.c
 * @brief Host-only FSM check: gcc -I desktop_pet/main/source/pet/pet_core desktop_pet/main/source/pet/pet_core/pet_core.c tools/pet_sim/pet_core_smoke.c
 */

#include "pet_core.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

int main(void)
{
    pet_needs_t n;
    pet_needs_cfg_t cfg = *pet_core_default_cfg();
    pet_intent_t in;
    int saw_clip = 0;

    cfg.hunger_decay_s = 2;
    cfg.mood_decay_s = 2;
    cfg.energy_decay_s = 2;
    pet_core_init(&cfg);

    while (pet_core_take_intent(&in)) {
        if (in.id == PET_INTENT_CLIP) {
            saw_clip = 1;
        }
    }
    if (!saw_clip) {
        fail("init should emit CLIP");
    }
    if (pet_core_clip() != PET_CLIP_IDLE) {
        fail("start idle");
    }

    if (!pet_core_post(PET_EVT_CARE_FEED, 0)) {
        fail("post feed");
    }
    pet_core_poll();
    if (pet_core_clip() != PET_CLIP_EAT) {
        fail("feed -> eat");
    }
    pet_core_get_needs(&n);
    if (n.hunger <= 70U) {
        fail("feed should raise hunger");
    }

    /* eat is priority 20: poke must not interrupt */
    (void)pet_core_post(PET_EVT_TOUCH_TAP, 0);
    pet_core_poll();
    if (pet_core_clip() != PET_CLIP_EAT) {
        fail("poke must not interrupt eat");
    }

    {
        int i;
        for (i = 0; i < 4; i++) {
            (void)pet_core_post(PET_EVT_TICK_1S, 0);
            pet_core_poll();
        }
    }
    if (pet_core_clip() == PET_CLIP_EAT) {
        fail("eat should finish");
    }

    (void)pet_core_post(PET_EVT_CARE_SLEEP, 0);
    pet_core_poll();
    pet_core_get_needs(&n);
    if (!n.sleeping || (pet_core_clip() != PET_CLIP_SLEEP_LOOP)) {
        fail("sleep");
    }
    (void)pet_core_post(PET_EVT_TOUCH_TAP, 0);
    pet_core_poll();
    pet_core_get_needs(&n);
    if (n.sleeping) {
        fail("tap wakes");
    }

    printf("pet_core_smoke ok clip=%s face=%s h=%u m=%u e=%u\n",
           pet_clip_name(pet_core_clip()),
           pet_face_name(pet_core_face()),
           (unsigned)n.hunger, (unsigned)n.mood, (unsigned)n.energy);
    return 0;
}
