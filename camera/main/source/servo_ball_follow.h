/**
 * @file servo_ball_follow.h
 * @brief 平衡杠跟球：画面内轻跟；出画 L↔R 极限摆寻球。
 */

#ifndef CAMERA_SERVO_BALL_FOLLOW_H
#define CAMERA_SERVO_BALL_FOLLOW_H

#include "type.h"

status_t servo_ball_follow_start(void);
void servo_ball_follow_set_enabled(bool_t on);
bool_t servo_ball_follow_is_enabled(void);

#endif /* CAMERA_SERVO_BALL_FOLLOW_H */
