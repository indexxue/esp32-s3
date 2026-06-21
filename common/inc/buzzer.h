/**
 * @file buzzer.h
 * @brief 蜂鸣器驱动：支持有源（GPIO 电平）与无源（LEDC PWM 方波）。
 */

#ifndef COMMON_BUZZER_H
#define COMMON_BUZZER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "type.h"

#include "pwm.h"

typedef enum {
    BUZZER_TYPE_ACTIVE = 0,  /**< 有源：高/低电平直接驱动 */
    BUZZER_TYPE_PASSIVE = 1, /**< 无源：PWM 输出 2~4 kHz 方波 */
} buzzer_type_e;

typedef enum {
    BUZZER_PATTERN_SHORT = 0,        /**< 短鸣 1 声（进菜单等） */
    BUZZER_PATTERN_DOUBLE_SHORT,     /**< 短鸣 2 声（投票开始） */
    BUZZER_PATTERN_TRIPLE_LONG,      /**< 长鸣 3 声（投票结束） */
    BUZZER_PATTERN_ALARM,            /**< 连续报警，直至 buzzer_stop_pattern */
} buzzer_pattern_e;

typedef struct {
    s32_t gpio;
    buzzer_type_e type;
    /** 有源：响时输出电平（通常 1）；无源时忽略 */
    u8_t active_level;
    /** 无源专用：PWM 定时器 / 通道 */
    PwmTimer_t pwm_timer;
    PwmChannel_t pwm_channel;
    /** 无源驱动频率（Hz），0 表示默认 3000 Hz */
    u32_t passive_freq_hz;
    /** 无源占空比，0 表示 50% */
    u32_t passive_duty;
} buzzer_config_t;

/** 初始化蜂鸣器；重复调用会先 deinit 再配置。 */
status_t buzzer_init(const buzzer_config_t *config);

void buzzer_deinit(void);

bool_t buzzer_is_ready(void);

/** 立即鸣响 / 静音（阻塞式底层控制）。 */
void buzzer_on(void);
void buzzer_off(void);

/** 非阻塞播放预设节奏；若忙则取消当前节奏并播放新节奏。 */
status_t buzzer_play_pattern(buzzer_pattern_e pattern);

/** 停止当前节奏或连续报警。 */
void buzzer_stop_pattern(void);

bool_t buzzer_pattern_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_BUZZER_H */
