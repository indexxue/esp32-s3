#ifndef COMMON_LED_SCENE_H
#define COMMON_LED_SCENE_H

#include "error.h"

typedef enum {
    LED_SCENE_OFF = 0,
    LED_SCENE_ON,
    LED_SCENE_BLINK,
    LED_SCENE_BREATHE
} led_scene_mode_t;

common_err_t led_scene_init(void);
common_err_t led_scene_set(led_scene_mode_t mode);

#endif
