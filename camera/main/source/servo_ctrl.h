/**
 * @file servo_ctrl.h
 * @brief camera 双路 MG996R：角度/脉宽换算 + LEDC 输出（Pan/Tilt）。
 */

#ifndef CAMERA_SERVO_CTRL_H
#define CAMERA_SERVO_CTRL_H

#include "type.h"

typedef enum {
    SERVO_CH_PAN  = 0,
    SERVO_CH_TILT = 1
} servo_ch_t;

status_t servo_init(void);
bool_t   servo_is_ready(void);
status_t servo_set_angle(servo_ch_t ch, float deg);
status_t servo_set_pulse_us(servo_ch_t ch, uint16_t us);
status_t servo_get_angle(servo_ch_t ch, float *deg);
status_t servo_center_all(void);
/** 相对当前角度步进（度），受软限位裁剪。 */
status_t servo_nudge(servo_ch_t ch, float delta_deg);

#endif /* CAMERA_SERVO_CTRL_H */
