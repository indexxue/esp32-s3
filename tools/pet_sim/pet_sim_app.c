/**
 * @file pet_sim_app.c
 * @brief Host port: stdio pack root + keyboard 1/2/3/4 care events.
 */

#include "pet_sim_app.h"

#include "pet_core.h"
#include "pet_fs.h"
#include "pet_res.h"
#include "pet_view.h"

#include "lvgl.h"

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void key_timer_cb(lv_timer_t *t)
{
#ifdef _WIN32
    static int s_prev[5];
    int down[5];
    int i;
    const int vk[5] = {'1', '2', '3', '4', '0'};
    const pet_evt_id_t ev[5] = {
        PET_EVT_CARE_FEED,
        PET_EVT_CARE_PLAY,
        PET_EVT_CARE_SLEEP,
        PET_EVT_CARE_WAKE,
        PET_EVT_IMU_SHAKE,
    };

    (void)t;
    for (i = 0; i < 5; i++) {
        down[i] = (GetAsyncKeyState(vk[i]) & 0x8000) ? 1 : 0;
        if (down[i] && !s_prev[i]) {
            (void)pet_core_post(ev[i], 0);
        }
        s_prev[i] = down[i];
    }
#else
    (void)t;
#endif
}

void pet_sim_app_create(void)
{
    const char *root = getenv("PET_PACK_ROOT");

    if ((root == NULL) || (root[0] == '\0')) {
        root = "tools/pet_sim/sdcard/pet";
    }
    pet_fs_set_root(root);
    if (pet_res_load()) {
        pet_core_init(pet_res_needs_cfg());
    } else {
        pet_core_init(NULL);
    }
    pet_view_create(lv_screen_active());
    (void)lv_timer_create(key_timer_cb, 50, NULL);
}
