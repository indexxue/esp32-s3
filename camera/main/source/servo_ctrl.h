/**
 * @file servo_ctrl.h
 * @brief camera 双路 MG996R：角度/脉宽换算 + LEDC 输出（Pan/Tilt）。
 *
 * 角度坐标系：0–BOARD_SERVO_ANGLE_MAX_DEG（默认 360）线性映射到
 * BOARD_SERVO_PULSE_MIN_US–MAX_US；软限位运行时可改，供网页标定极限。
 */

#ifndef CAMERA_SERVO_CTRL_H
#define CAMERA_SERVO_CTRL_H

#include "type.h"

typedef enum {
    SERVO_CH_PAN  = 0,
    SERVO_CH_TILT = 1
} servo_ch_t;

typedef struct {
    float pan_min_deg;
    float pan_max_deg;
    float tilt_min_deg;
    float tilt_max_deg;
    float angle_max_deg; /* 脉宽映射满量程（编译期宏，只读回传） */
    float center_deg;
} servo_limits_t;

status_t servo_init(void);
bool_t   servo_is_ready(void);
status_t servo_set_angle(servo_ch_t ch, float deg);
status_t servo_set_pulse_us(servo_ch_t ch, uint16_t us);
status_t servo_get_angle(servo_ch_t ch, float *deg);
status_t servo_get_pulse_us(servo_ch_t ch, uint16_t *us);
status_t servo_center_all(void);
/** 相对当前角度步进（度），受软限位裁剪。 */
status_t servo_nudge(servo_ch_t ch, float delta_deg);

status_t servo_get_limits(servo_limits_t *out);
/**
 * 设置软限位（度）。各指针可为 NULL（该端点不变）；min 必须 < max，
 * 且落在 [0, BOARD_SERVO_ANGLE_MAX_DEG] 内。成功后若当前角越界则钳到新窗口。
 */
status_t servo_set_limits(const float *pan_min,
                          const float *pan_max,
                          const float *tilt_min,
                          const float *tilt_max);
/** 恢复 board.h 编译期默认软限位。 */
status_t servo_reset_limits(void);

#endif /* CAMERA_SERVO_CTRL_H */
