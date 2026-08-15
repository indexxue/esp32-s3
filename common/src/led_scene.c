/**
 * @file    led_scene.c
 * @brief   LED 场景状态机（逻辑参考 STM32F103 版）；输出经 WS2812B 全彩刷新。
 */

#include "led_scene.h"

#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ws2812b.h"
#include "ws2812b_esp32.h"

/** 板级 WS2812B：数据线 GPIO 与级联颗数（改硬件时只改此处） */
#ifndef LED_SCENE_WS2812_GPIO
#define LED_SCENE_WS2812_GPIO GPIO_NUM_48
#endif
#ifndef LED_SCENE_WS2812_NUM_LEDS
#define LED_SCENE_WS2812_NUM_LEDS 16U
#endif

static ws2812b_t s_ws2812;
static bool s_update_task_started;

typedef struct
{
    led_scene_id_e id;
    bool running;
    uint8_t current_cycle;
    uint8_t current_action;
    uint8_t action_cycle;
    uint32_t action_time;
    led_rgb_value_t current_rgb;
    uint8_t rainbow_hue; /* ACTION_RAINBOW 色相基准 0–255 */
} led_scene_state_t;

typedef struct
{
    led_scene_state_t states[LED_SCENE_ID_MAX_NUM];
    led_scene_id_e active_scene;
    bool initialized;
    ws2812b_t *strip;
} led_scene_self_t;

static led_scene_self_t self;

/* 配色参考：WS2812 上略降饱和可避免刺眼；配对场景偏「连接/蓝牙」冷色律动 */

static const led_scene_t led_scene_bootup = {
    .cycle = 1,
    .num = 5,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xC8,
        .sub.onoff.value.g = 0x28,
        .sub.onoff.value.b = 0x00,
        .sub.onoff.lifetime = 280 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xFF,
        .sub.onoff.value.g = 0x78,
        .sub.onoff.value.b = 0x08,
        .sub.onoff.lifetime = 320 * LED_SCENE_MSEC,
    },
    .action[2] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xFF,
        .sub.onoff.value.g = 0xB8,
        .sub.onoff.value.b = 0x40,
        .sub.onoff.lifetime = 380 * LED_SCENE_MSEC,
    },
    .action[3] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xFF,
        .sub.onoff.value.g = 0xDC,
        .sub.onoff.value.b = 0xA0,
        .sub.onoff.lifetime = 520 * LED_SCENE_MSEC,
    },
    .action[4] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0x40,
        .sub.onoff.value.g = 0x90,
        .sub.onoff.value.b = 0x38,
        .sub.onoff.lifetime = 450 * LED_SCENE_MSEC,
    },
};

static const led_scene_t led_scene_pairing = {
    .cycle = 36,
    .num = 6,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0x00,
        .sub.onoff.value.g = 0xD8,
        .sub.onoff.value.b = 0xF0,
        .sub.onoff.lifetime = 320 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 140 * LED_SCENE_MSEC,
    },
    .action[2] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0x20,
        .sub.onoff.value.g = 0x60,
        .sub.onoff.value.b = 0xFF,
        .sub.onoff.lifetime = 320 * LED_SCENE_MSEC,
    },
    .action[3] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 140 * LED_SCENE_MSEC,
    },
    .action[4] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xA0,
        .sub.onoff.value.g = 0x30,
        .sub.onoff.value.b = 0xFF,
        .sub.onoff.lifetime = 320 * LED_SCENE_MSEC,
    },
    .action[5] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 360 * LED_SCENE_MSEC,
    },
};

/** 触发：黄灯闪 5 次后自动结束 */
static const led_scene_t led_scene_trigger = {
    .cycle = 5,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xFF,
        .sub.onoff.value.g = 0xD0,
        .sub.onoff.value.b = 0x20,
        .sub.onoff.lifetime = 200 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 200 * LED_SCENE_MSEC,
    },
};

/** 异常 / 报警：红灯快闪，直至 `led_scene_cancel` */
static const led_scene_t led_scene_red_fast = {
    .cycle = CYCLE_ALWAYS,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xFF,
        .sub.onoff.value.g = 0x10,
        .sub.onoff.value.b = 0x08,
        .sub.onoff.lifetime = 100 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 100 * LED_SCENE_MSEC,
    },
};

/** 配网成功：绿灯常亮约 5s */
static const led_scene_t led_scene_success = {
    .cycle = 1,
    .num = 1,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0x00,
        .sub.onoff.value.g = 0xE8,
        .sub.onoff.value.b = 0x40,
        .sub.onoff.lifetime = 5000 * LED_SCENE_MSEC,
    },
};

/** 断网：红 / 蓝慢闪 */
static const led_scene_t led_scene_net_offline = {
    .cycle = CYCLE_ALWAYS,
    .num = 4,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xE0,
        .sub.onoff.value.g = 0x10,
        .sub.onoff.value.b = 0x10,
        .sub.onoff.lifetime = 550 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 400 * LED_SCENE_MSEC,
    },
    .action[2] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0x18,
        .sub.onoff.value.g = 0x50,
        .sub.onoff.value.b = 0xF0,
        .sub.onoff.lifetime = 550 * LED_SCENE_MSEC,
    },
    .action[3] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 400 * LED_SCENE_MSEC,
    },
};

/** 工作中：整条彩虹渐变 */
static const led_scene_t led_scene_working = {
    .cycle = CYCLE_ALWAYS,
    .num = 1,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_RAINBOW,
        .sub.rainbow.hue_step = 2,
        .sub.rainbow.saturation = 0xF0,
        .sub.rainbow.value = 0xB0,
    },
};

/** 回网：绿灯慢闪约 10s（10 个亮灭周期） */
static const led_scene_t led_scene_net_online = {
    .cycle = 10,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0x00,
        .sub.onoff.value.g = 0xC8,
        .sub.onoff.value.b = 0x38,
        .sub.onoff.lifetime = 550 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 450 * LED_SCENE_MSEC,
    },
};

/** 配置态：紫灯常亮 */
static const led_scene_t led_scene_config = {
    .cycle = CYCLE_ALWAYS,
    .num = 1,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_HOLD,
        .sub.hold.value.r = 0xA0,
        .sub.hold.value.g = 0x18,
        .sub.hold.value.b = 0xD8,
    },
};

/** 充电：黄灯常亮约 3s */
static const led_scene_t led_scene_charging = {
    .cycle = 1,
    .num = 1,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xFF,
        .sub.onoff.value.g = 0xC8,
        .sub.onoff.value.b = 0x28,
        .sub.onoff.lifetime = 3000 * LED_SCENE_MSEC,
    },
};

/** 低电：黄灯短亮一次，约 60s 周期 */
static const led_scene_t led_scene_low_battery = {
    .cycle = CYCLE_ALWAYS,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xFF,
        .sub.onoff.value.g = 0xC0,
        .sub.onoff.value.b = 0x20,
        .sub.onoff.lifetime = 250 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 59750 * LED_SCENE_MSEC,
    },
};

/** ballot_guard：待机 / 锁定 — 绿色常亮 */
static const led_scene_t led_scene_ballot_idle = {
    .cycle = CYCLE_ALWAYS,
    .num = 1,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_HOLD,
        .sub.hold.value.r = 0x00,
        .sub.hold.value.g = 0xE8,
        .sub.hold.value.b = 0x40,
    },
};

/** ballot_guard：投票开始提示 — 蓝色慢闪 1 Hz × 5 s（5 个亮灭周期） */
static const led_scene_t led_scene_ballot_voting = {
    .cycle = 5,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0x18,
        .sub.onoff.value.g = 0x50,
        .sub.onoff.value.b = 0xF0,
        .sub.onoff.lifetime = 500 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 500 * LED_SCENE_MSEC,
    },
};

/** ballot_guard：红外靠近 — 蓝色快闪 2 Hz 约 2 s（4 个亮灭周期） */
static const led_scene_t led_scene_ballot_approach = {
    .cycle = 4,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0x30,
        .sub.onoff.value.g = 0x70,
        .sub.onoff.value.b = 0xFF,
        .sub.onoff.lifetime = 250 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 250 * LED_SCENE_MSEC,
    },
};

/** ballot_guard：有效票 — 绿色快闪 2 次 */
static const led_scene_t led_scene_ballot_valid = {
    .cycle = 2,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0x00,
        .sub.onoff.value.g = 0xE8,
        .sub.onoff.value.b = 0x40,
        .sub.onoff.lifetime = 200 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 200 * LED_SCENE_MSEC,
    },
};

/** ballot_guard：废票 — 红色快闪 3 次 */
static const led_scene_t led_scene_ballot_spoiled = {
    .cycle = 3,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xFF,
        .sub.onoff.value.g = 0x20,
        .sub.onoff.value.b = 0x10,
        .sub.onoff.lifetime = 200 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 200 * LED_SCENE_MSEC,
    },
};

/** ballot_guard：违规 — 红色急促闪（80 ms 周期） */
static const led_scene_t led_scene_ballot_violation = {
    .cycle = CYCLE_ALWAYS,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0xFF,
        .sub.onoff.value.g = 0x10,
        .sub.onoff.value.b = 0x08,
        .sub.onoff.lifetime = 80 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_ONOFF,
        .sub.onoff.value.r = 0,
        .sub.onoff.value.g = 0,
        .sub.onoff.value.b = 0,
        .sub.onoff.lifetime = 80 * LED_SCENE_MSEC,
    },
};

/** ballot_guard：系统异常 — 红色常亮 */
static const led_scene_t led_scene_ballot_fault = {
    .cycle = CYCLE_ALWAYS,
    .num = 1,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_HOLD,
        .sub.hold.value.r = 0xFF,
        .sub.hold.value.g = 0x10,
        .sub.hold.value.b = 0x08,
    },
};

/** ballot_guard：Wi-Fi STA 离线 — 黄色常亮 */
static const led_scene_t led_scene_ballot_wifi_warn = {
    .cycle = CYCLE_ALWAYS,
    .num = 1,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_HOLD,
        .sub.hold.value.r = 0xFF,
        .sub.hold.value.g = 0xC8,
        .sub.hold.value.b = 0x28,
    },
};

/** desktop_pet：聆听 — 青色慢呼吸（暗↔亮） */
static const led_scene_t led_scene_pet_listen = {
    .cycle = CYCLE_ALWAYS,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_FADE,
        .sub.fade.start_value.r = 0x00,
        .sub.fade.start_value.g = 0x28,
        .sub.fade.start_value.b = 0x40,
        .sub.fade.end_value.r = 0x00,
        .sub.fade.end_value.g = 0xD0,
        .sub.fade.end_value.b = 0xF0,
        .sub.fade.step = 8,
        .sub.fade.interval = 50 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_FADE,
        .sub.fade.start_value.r = 0x00,
        .sub.fade.start_value.g = 0xD0,
        .sub.fade.start_value.b = 0xF0,
        .sub.fade.end_value.r = 0x00,
        .sub.fade.end_value.g = 0x28,
        .sub.fade.end_value.b = 0x40,
        .sub.fade.step = 8,
        .sub.fade.interval = 50 * LED_SCENE_MSEC,
    },
};

/** desktop_pet：回答 — 暖橙慢呼吸（暗↔亮） */
static const led_scene_t led_scene_pet_speak = {
    .cycle = CYCLE_ALWAYS,
    .num = 2,
    .action[0] = {
        .cycle = 1,
        .type = ACTION_FADE,
        .sub.fade.start_value.r = 0x40,
        .sub.fade.start_value.g = 0x18,
        .sub.fade.start_value.b = 0x00,
        .sub.fade.end_value.r = 0xFF,
        .sub.fade.end_value.g = 0x90,
        .sub.fade.end_value.b = 0x20,
        .sub.fade.step = 10,
        .sub.fade.interval = 40 * LED_SCENE_MSEC,
    },
    .action[1] = {
        .cycle = 1,
        .type = ACTION_FADE,
        .sub.fade.start_value.r = 0xFF,
        .sub.fade.start_value.g = 0x90,
        .sub.fade.start_value.b = 0x20,
        .sub.fade.end_value.r = 0x40,
        .sub.fade.end_value.g = 0x18,
        .sub.fade.end_value.b = 0x00,
        .sub.fade.step = 10,
        .sub.fade.interval = 40 * LED_SCENE_MSEC,
    },
};

static const led_scene_tab_t scene_table[LED_SCENE_ID_MAX_NUM] = {
    [LED_SCENE_ID_BOOTUP]      = {.prio = LED_SCENE_PRIO_LOW,    .scene = &led_scene_bootup,},
    [LED_SCENE_ID_PAIRING]     = {.prio = LED_SCENE_PRIO_PAIR,   .scene = &led_scene_pairing,},
    [LED_SCENE_ID_TRIGGER]     = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_trigger,},
    [LED_SCENE_ID_ERROR]       = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_red_fast,},
    [LED_SCENE_ID_SUCCESS]     = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_success,},
    [LED_SCENE_ID_NET_OFFLINE] = {.prio = LED_SCENE_PRIO_LOW,    .scene = &led_scene_net_offline,},
    [LED_SCENE_ID_WORKING]     = {.prio = LED_SCENE_PRIO_LOW,    .scene = &led_scene_working,},
    [LED_SCENE_ID_ALARM]       = {.prio = LED_SCENE_PRIO_PAIR,   .scene = &led_scene_red_fast,},
    [LED_SCENE_ID_NET_ONLINE]  = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_net_online,},
    [LED_SCENE_ID_CONFIG]      = {.prio = LED_SCENE_PRIO_PAIR,   .scene = &led_scene_config,},
    [LED_SCENE_ID_CHARGING]    = {.prio = LED_SCENE_PRIO_LOW,    .scene = &led_scene_charging,},
    [LED_SCENE_ID_LOW_BATTERY] = {.prio = LED_SCENE_PRIO_LOW,    .scene = &led_scene_low_battery,},
    [LED_SCENE_ID_BALLOT_IDLE]      = {.prio = LED_SCENE_PRIO_LOW,    .scene = &led_scene_ballot_idle,},
    [LED_SCENE_ID_BALLOT_VOTING]    = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_ballot_voting,},
    [LED_SCENE_ID_BALLOT_APPROACH]  = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_ballot_approach,},
    [LED_SCENE_ID_BALLOT_VALID]     = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_ballot_valid,},
    [LED_SCENE_ID_BALLOT_SPOILED]   = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_ballot_spoiled,},
    [LED_SCENE_ID_BALLOT_VIOLATION] = {.prio = LED_SCENE_PRIO_PAIR,   .scene = &led_scene_ballot_violation,},
    [LED_SCENE_ID_BALLOT_FAULT]     = {.prio = LED_SCENE_PRIO_FACTORY,.scene = &led_scene_ballot_fault,},
    [LED_SCENE_ID_BALLOT_WIFI_WARN] = {.prio = LED_SCENE_PRIO_LOW,    .scene = &led_scene_ballot_wifi_warn,},
    [LED_SCENE_ID_PET_LISTEN]       = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_pet_listen,},
    [LED_SCENE_ID_PET_SPEAK]        = {.prio = LED_SCENE_PRIO_NORMAL, .scene = &led_scene_pet_speak,},
};

static uint8_t led_scene_scale8(uint8_t c, uint8_t scale)
{
    return (uint8_t)(((uint16_t)c * (uint16_t)scale) >> 8);
}

static led_rgb_value_t led_scene_scale_rgb(led_rgb_value_t rgb)
{
    const uint8_t s = (uint8_t)LED_SCENE_BRIGHTNESS_SCALE;
    rgb.r           = led_scene_scale8(rgb.r, s);
    rgb.g           = led_scene_scale8(rgb.g, s);
    rgb.b           = led_scene_scale8(rgb.b, s);
    return rgb;
}

/** 色相 pos 0–255，饱和全开的色环段 */
static void led_scene_wheel_rgb(uint8_t pos, uint8_t *r, uint8_t *g, uint8_t *b)
{
    pos = (uint8_t)(255U - pos);
    if (pos < 85U)
    {
        *r = (uint8_t)(255U - pos * 3U);
        *g = 0U;
        *b = (uint8_t)(pos * 3U);
    }
    else if (pos < 170U)
    {
        pos = (uint8_t)(pos - 85U);
        *r = 0U;
        *g = (uint8_t)(pos * 3U);
        *b = (uint8_t)(255U - pos * 3U);
    }
    else
    {
        pos = (uint8_t)(pos - 170U);
        *r = (uint8_t)(pos * 3U);
        *g = (uint8_t)(255U - pos * 3U);
        *b = 0U;
    }
}

static void led_scene_output_rainbow(ws2812b_t *dev, uint8_t base_hue, uint8_t sat, uint8_t val)
{
    if (dev == NULL || !ws2812b_is_initialized(dev))
    {
        return;
    }
    const uint16_t n = ws2812b_get_num_leds(dev);
    if (n == 0U)
    {
        return;
    }
    const uint8_t amp = (uint8_t)(((uint16_t)sat * (uint16_t)val) / 255U);
    const uint16_t denom = (n > 1U) ? (uint16_t)(n - 1U) : 1U;
    for (uint16_t i = 0; i < n; i++)
    {
        const uint8_t h =
            (uint8_t)(base_hue + (uint16_t)((uint32_t)i * 255U / denom));
        uint8_t r0;
        uint8_t g0;
        uint8_t b0;
        led_scene_wheel_rgb(h, &r0, &g0, &b0);
        r0 = led_scene_scale8(r0, amp);
        g0 = led_scene_scale8(g0, amp);
        b0 = led_scene_scale8(b0, amp);
        r0 = led_scene_scale8(r0, (uint8_t)LED_SCENE_BRIGHTNESS_SCALE);
        g0 = led_scene_scale8(g0, (uint8_t)LED_SCENE_BRIGHTNESS_SCALE);
        b0 = led_scene_scale8(b0, (uint8_t)LED_SCENE_BRIGHTNESS_SCALE);
        (void)ws2812b_set_pixel_rgb(dev, i, r0, g0, b0);
    }
    (void)ws2812b_refresh(dev);
}

/** @return true 当该通道已到达 end */
static bool led_scene_fade_step_channel(uint8_t *cur, uint8_t end, uint8_t step)
{
    if (*cur == end)
    {
        return true;
    }
    if (*cur < end)
    {
        if ((uint16_t)*cur + step >= end)
        {
            *cur = end;
            return true;
        }
        *cur = (uint8_t)(*cur + step);
        return false;
    }
    if (*cur <= step || *cur - step <= end)
    {
        *cur = end;
        return true;
    }
    *cur = (uint8_t)(*cur - step);
    return false;
}

static void led_scene_output(const led_rgb_value_t *rgb)
{
    ws2812b_t *dev = self.strip;
    if (dev == NULL || !ws2812b_is_initialized(dev))
    {
        return;
    }

    if (rgb == NULL)
    {
        (void)ws2812b_clear(dev);
        return;
    }

    const led_rgb_value_t dim = led_scene_scale_rgb(*rgb);
    const uint16_t n          = ws2812b_get_num_leds(dev);
    for (uint16_t i = 0; i < n; i++)
    {
        (void)ws2812b_set_pixel_rgb(dev, i, dim.r, dim.g, dim.b);
    }
    (void)ws2812b_refresh(dev);
}

static led_scene_id_e led_scene_find_highest_priority(void)
{
    led_scene_id_e highest_id = LED_SCENE_ID_MAX_NUM;
    led_scene_prio_e highest_prio = LED_SCENE_PRIO_MAX_NUM;

    for (uint8_t i = 0; i < LED_SCENE_ID_MAX_NUM; i++)
    {
        if (self.states[i].running)
        {
            led_scene_prio_e prio = scene_table[i].prio;
            if (prio < highest_prio)
            {
                highest_prio = prio;
                highest_id = (led_scene_id_e)i;
            }
        }
    }

    return highest_id;
}

void led_scene_update(void)
{
    if (!self.initialized || self.strip == NULL)
    {
        return;
    }

    if (self.active_scene >= LED_SCENE_ID_MAX_NUM)
    {
        return;
    }

    led_scene_state_t *state = &self.states[self.active_scene];
    const led_scene_t *scene = scene_table[self.active_scene].scene;

    if (!state->running || scene == NULL)
    {
        return;
    }

    state->action_time += LED_SCENE_TICK_MS;

    const led_scene_action_t *action = &scene->action[state->current_action];
    bool action_complete = false;

    if (action->type == ACTION_ONOFF)
    {
        if (state->action_time >= action->sub.onoff.lifetime)
        {
            action_complete = true;
        }
        else
        {
            led_scene_output(&action->sub.onoff.value);
        }
    }
    else if (action->type == ACTION_FADE)
    {
        if (state->action_time >= action->sub.fade.interval)
        {
            const uint8_t step = action->sub.fade.step;
            const bool r_done =
                led_scene_fade_step_channel(&state->current_rgb.r, action->sub.fade.end_value.r, step);
            const bool g_done =
                led_scene_fade_step_channel(&state->current_rgb.g, action->sub.fade.end_value.g, step);
            const bool b_done =
                led_scene_fade_step_channel(&state->current_rgb.b, action->sub.fade.end_value.b, step);
            const bool fade_complete = r_done && g_done && b_done;

            led_scene_output(&state->current_rgb);
            state->action_time = 0;

            if (fade_complete)
            {
                action_complete = true;
            }
        }
    }
    else if (action->type == ACTION_HOLD)
    {
        led_scene_output(&action->sub.hold.value);
    }
    else if (action->type == ACTION_RAINBOW)
    {
        state->rainbow_hue =
            (uint8_t)(state->rainbow_hue + action->sub.rainbow.hue_step);
        led_scene_output_rainbow(self.strip, state->rainbow_hue, action->sub.rainbow.saturation,
                                 action->sub.rainbow.value);
    }

    if (action_complete)
    {
        state->action_cycle++;
        if (state->action_cycle >= action->cycle)
        {
            state->action_cycle = 0;
            state->current_action++;
            if (state->current_action >= scene->num)
            {
                state->current_action = 0;
                state->current_cycle++;
                if (scene->cycle != CYCLE_ALWAYS && state->current_cycle >= scene->cycle)
                {
                    state->running = false;
                    led_scene_output(NULL);
                    self.active_scene = led_scene_find_highest_priority();
                    if (self.active_scene >= LED_SCENE_ID_MAX_NUM)
                    {
                        return;
                    }
                    state = &self.states[self.active_scene];
                    scene = scene_table[self.active_scene].scene;
                }
            }
            if (state->running && scene != NULL)
            {
                const led_scene_action_t *next_a = &scene->action[state->current_action];
                if (next_a->type == ACTION_FADE)
                {
                    state->current_rgb = next_a->sub.fade.start_value;
                }
                else if (next_a->type == ACTION_RAINBOW)
                {
                    state->rainbow_hue = 0;
                }
            }
        }
        else if (action->type == ACTION_FADE)
        {
            state->current_rgb = action->sub.fade.start_value;
        }
        state->action_time = 0;
    }
}

status_t led_scene_init(void)
{
    memset(&self, 0, sizeof(led_scene_self_t));

    const ws2812b_esp32_config_t cfg = {
        .gpio_num          = LED_SCENE_WS2812_GPIO,
        .num_leds          = LED_SCENE_WS2812_NUM_LEDS,
        .resolution_hz     = 0,
        .mem_block_symbols = 0,
        .trans_queue_depth = 0,
    };

    if (ws2812b_esp32_init(&s_ws2812, &cfg) != ESP_OK)
    {
        return STATUS_FAIL;
    }

    self.strip = &s_ws2812;
    self.active_scene = LED_SCENE_ID_MAX_NUM;
    self.initialized = true;

    return STATUS_OK;
}

static void led_scene_update_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(LED_SCENE_TICK_MS);
    for (;;)
    {
        led_scene_update();
        vTaskDelay(period);
    }
}

status_t led_scene_start_update_task(void)
{
    if (!self.initialized)
    {
        return STATUS_INVALID_STATE;
    }
    if (s_update_task_started)
    {
        return STATUS_OK;
    }
    if (xTaskCreate(led_scene_update_task, "led_scene", 2048, NULL, 5, NULL) != pdPASS)
    {
        return STATUS_NO_MEM;
    }
    s_update_task_started = true;
    return STATUS_OK;
}

void led_scene_led_direct_set(led_scene_led_e led, bool on)
{
    if (!self.initialized || self.strip == NULL || !ws2812b_is_initialized(self.strip))
    {
        return;
    }

    ws2812b_t *dev = self.strip;
    const uint16_t n = ws2812b_get_num_leds(dev);

    /* 直接点灯：暖琥珀 + 冰青，避免纯红/纯蓝刺眼 */
    if (led == LED_SCENE_LED_0 && n > 0U)
    {
        const led_rgb_value_t c = led_scene_scale_rgb((led_rgb_value_t){on ? 0xFFu : 0u, on ? 0xC8u : 0u, on ? 0x58u : 0u});
        (void)ws2812b_set_pixel_rgb(dev, 0, c.r, c.g, c.b);
    }
    else if (led == LED_SCENE_LED_1 && n > 1U)
    {
        const led_rgb_value_t c = led_scene_scale_rgb((led_rgb_value_t){on ? 0x38u : 0u, on ? 0xD0u : 0u, on ? 0xFFu : 0u});
        (void)ws2812b_set_pixel_rgb(dev, 1, c.r, c.g, c.b);
    }
    else if (led == LED_SCENE_LED_2 && n > 2U)
    {
        const led_rgb_value_t c = led_scene_scale_rgb((led_rgb_value_t){on ? 0xE8u : 0u, on ? 0x40u : 0u, on ? 0x90u : 0u});
        (void)ws2812b_set_pixel_rgb(dev, 2, c.r, c.g, c.b);
    }

    (void)ws2812b_refresh(dev);
}

static void led_scene_reset_state(led_scene_state_t *state, const led_scene_t *scene)
{
    state->running = true;
    state->current_cycle = 0;
    state->current_action = 0;
    state->action_cycle = 0;
    state->action_time = 0;
    state->rainbow_hue = 0;
    state->current_rgb.r = 0;
    state->current_rgb.g = 0;
    state->current_rgb.b = 0;
    if (scene != NULL && scene->num > 0U)
    {
        const led_scene_action_t *first = &scene->action[0];
        if (first->type == ACTION_FADE)
        {
            state->current_rgb = first->sub.fade.start_value;
        }
    }
}

void led_scene_run(led_scene_id_e id)
{
    if (id >= LED_SCENE_ID_MAX_NUM)
    {
        return;
    }

    if (!self.initialized)
    {
        return;
    }

    led_scene_state_t *state = &self.states[id];
    const led_scene_t *scene = scene_table[id].scene;

    if (scene == NULL)
    {
        return;
    }

    if (state->running)
    {
        return;
    }

    led_scene_reset_state(state, scene);

    led_scene_id_e new_scene = led_scene_find_highest_priority();
    if (new_scene != self.active_scene)
    {
        if (self.active_scene < LED_SCENE_ID_MAX_NUM)
        {
            self.states[self.active_scene].running = false;
        }
        self.active_scene = new_scene;
    }

    if (self.active_scene < LED_SCENE_ID_MAX_NUM)
    {
        led_scene_reset_state(&self.states[self.active_scene], scene_table[self.active_scene].scene);
    }
}

void led_scene_run_force(led_scene_id_e id)
{
    if (id >= LED_SCENE_ID_MAX_NUM)
    {
        return;
    }

    if (!self.initialized)
    {
        return;
    }

    const led_scene_t *scene = scene_table[id].scene;
    if (scene == NULL)
    {
        return;
    }

    for (uint8_t i = 0; i < LED_SCENE_ID_MAX_NUM; i++)
    {
        if (i != id)
        {
            self.states[i].running = false;
        }
    }

    self.active_scene = id;
    led_scene_reset_state(&self.states[id], scene);
}

void led_scene_cancel(led_scene_id_e id)
{
    if (id >= LED_SCENE_ID_MAX_NUM)
    {
        return;
    }

    if (!self.initialized)
    {
        return;
    }

    led_scene_state_t *state = &self.states[id];
    state->running = false;

    if (self.active_scene == id)
    {
        led_scene_output(NULL);
        led_scene_id_e new_scene = led_scene_find_highest_priority();
        if (new_scene < LED_SCENE_ID_MAX_NUM)
        {
            self.active_scene = new_scene;
            led_scene_state_t *active_state = &self.states[self.active_scene];
            active_state->current_cycle = 0;
            active_state->current_action = 0;
            active_state->action_cycle = 0;
            active_state->action_time = 0;
            active_state->rainbow_hue = 0;
            active_state->current_rgb.r = 0;
            active_state->current_rgb.g = 0;
            active_state->current_rgb.b = 0;
            const led_scene_action_t *first = &scene_table[self.active_scene].scene->action[0];
            if (first->type == ACTION_FADE)
            {
                active_state->current_rgb = first->sub.fade.start_value;
            }
        }
        else
        {
            self.active_scene = LED_SCENE_ID_MAX_NUM;
        }
    }
}
