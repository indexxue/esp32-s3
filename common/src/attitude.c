/**
 * @file attitude.c
 * @brief 姿态角度计算与互补滤波实现。
 */

#include "attitude.h"

#include <math.h>

#define ATTITUDE_RAD_TO_DEG_F (180.0f / 3.14159265f)

void attitude_tilt_from_accel_raw_deg(int16_t ax, int16_t ay, int16_t az, float *roll_deg, float *pitch_deg)
{
    if (roll_deg == NULL || pitch_deg == NULL) {
        return;
    }

    const float fx = (float)ax;
    const float fy = (float)ay;
    const float fz = (float)az;

    *roll_deg  = atan2f(fy, fz) * ATTITUDE_RAD_TO_DEG_F;
    *pitch_deg = atan2f(-fx, sqrtf(fy * fy + fz * fz)) * ATTITUDE_RAD_TO_DEG_F;
}

float attitude_qmi8658a_gyro_raw_to_dps(int16_t gyro_raw, uint8_t qmi8658a_gyro_gfs)
{
    /* QMI8658 典型灵敏度：LSB/(°/s)，与 gfs 档位 0..7 对应（见器件手册 CTRL3）。 */
    static const float s_lsb_per_dps[8] = {
        2048.0f, 1024.0f, 512.0f, 256.0f, 128.0f, 64.0f, 32.0f, 16.0f,
    };

    if (qmi8658a_gyro_gfs > 7u) {
        return 0.0f;
    }

    return (float)gyro_raw / s_lsb_per_dps[qmi8658a_gyro_gfs];
}

void attitude_complementary_deg_init(attitude_complementary_deg_t *st, float alpha)
{
    if (st == NULL) {
        return;
    }

    st->roll_deg  = 0.0f;
    st->pitch_deg = 0.0f;
    st->alpha     = alpha;
    st->inited    = false;
}

void attitude_complementary_deg_update(attitude_complementary_deg_t *st,
                                       float                         dt_s,
                                       float                         roll_acc_deg,
                                       float                         pitch_acc_deg,
                                       float                         gx_dps,
                                       float                         gy_dps)
{
    if (st == NULL || dt_s <= 0.0f) {
        return;
    }

    if (!st->inited) {
        st->roll_deg  = roll_acc_deg;
        st->pitch_deg = pitch_acc_deg;
        st->inited    = true;
        return;
    }

    float a = st->alpha;
    if (a < 0.0f) {
        a = 0.0f;
    }
    if (a > 1.0f) {
        a = 1.0f;
    }

    const float roll_gyro  = st->roll_deg + gx_dps * dt_s;
    const float pitch_gyro = st->pitch_deg + gy_dps * dt_s;

    st->roll_deg  = a * roll_gyro + (1.0f - a) * roll_acc_deg;
    st->pitch_deg = a * pitch_gyro + (1.0f - a) * pitch_acc_deg;
}

float attitude_fused_roll_deg(const attitude_complementary_deg_t *st)
{
    if (st == NULL || !st->inited) {
        return 0.0f;
    }
    return st->roll_deg;
}

float attitude_fused_pitch_deg(const attitude_complementary_deg_t *st)
{
    if (st == NULL || !st->inited) {
        return 0.0f;
    }
    return st->pitch_deg;
}
