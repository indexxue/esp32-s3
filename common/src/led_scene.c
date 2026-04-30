#include "led_scene.h"

static led_scene_mode_t s_mode = LED_SCENE_OFF;

common_err_t led_scene_init(void)
{
    s_mode = LED_SCENE_OFF;
    return COMMON_OK;
}

common_err_t led_scene_set(led_scene_mode_t mode)
{
    s_mode = mode;
    (void)s_mode;
    return COMMON_OK;
}
