/**
 * @file attitude.h
 * @brief 应用层姿态角度：由 IMU raw 推算倾斜角（度），及 QMI8658A 陀螺典型标度与一阶互补融合。
 *
 * 传感器驱动仍在 cbb，本模块仅做数学与滤波，不移动 cbb 源码。
 */

#ifndef COMMON_ATTITUDE_H
#define COMMON_ATTITUDE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 由加速度计 raw 估算倾斜角（度）：Roll 绕 X，Pitch 绕 Y；无磁力计时无航向。
 *        静止或低速时较有意义，大幅运动时受线加速度干扰。
 */
void attitude_tilt_from_accel_raw_deg(int16_t ax, int16_t ay, int16_t az, float *roll_deg, float *pitch_deg);

/**
 * @brief QMI8658A CTRL3 中 gyro 全量程档位 gfs（0..7），与 `qmi8658a_gyro_range_t` 枚举值一致。
 * @return 角速度（°/s），未知档位时返回 0。
 */
float attitude_qmi8658a_gyro_raw_to_dps(int16_t gyro_raw, uint8_t qmi8658a_gyro_gfs);

/** 一阶互补滤波：alpha 为陀螺积分权重（0~1），越大越信陀螺、加速度修正越慢。 */
typedef struct {
    float roll_deg;
    float pitch_deg;
    float alpha;
    bool  inited;
} attitude_complementary_deg_t;

void attitude_complementary_deg_init(attitude_complementary_deg_t *st, float alpha);

/**
 * @brief 融合加速度 tilt 与陀螺（°/s）；首帧仅用加速度初始化内部角度。
 */
void attitude_complementary_deg_update(attitude_complementary_deg_t *st,
                                       float                         dt_s,
                                       float                         roll_acc_deg,
                                       float                         pitch_acc_deg,
                                       float                         gx_dps,
                                       float                         gy_dps);

float attitude_fused_roll_deg(const attitude_complementary_deg_t *st);
float attitude_fused_pitch_deg(const attitude_complementary_deg_t *st);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_ATTITUDE_H */
