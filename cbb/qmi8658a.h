/**
 * @file    qmi8658a.h
 * @brief   Generic QMI8658A / QMI8658 6-axis IMU (I2C + delay callbacks, no MCU dependencies).
 *
 * Typical 7-bit I2C address: 0x6A (SA0 low) or 0x6B (SA0 high). WHO_AM_I = 0x05.
 */

#ifndef QMI8658A_H
#define QMI8658A_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*qmi8658a_i2c_write_t)(uint8_t addr, const uint8_t *data, uint16_t len);
typedef int (*qmi8658a_i2c_read_t)(uint8_t addr, uint8_t *data, uint16_t len);
/** 可选：一次事务内先写再读（如 I2C repeated start）。非 NULL 时读寄存器优先走此路径。 */
typedef int (*qmi8658a_i2c_write_read_t)(uint8_t addr,
                                       const uint8_t *write_data,
                                       uint16_t write_len,
                                       uint8_t *read_data,
                                       uint16_t read_len);
typedef void (*qmi8658a_delay_ms_t)(uint32_t ms);

typedef enum {
    QMI8658A_OK = 0,
    QMI8658A_ERROR_I2C,
    QMI8658A_ERROR_NOT_INIT,
    QMI8658A_ERROR_PARAM,
    QMI8658A_ERROR_ID
} qmi8658a_status_t;

typedef enum {
    QMI8658A_ACCEL_RANGE_2G  = 0u,
    QMI8658A_ACCEL_RANGE_4G  = 1u,
    QMI8658A_ACCEL_RANGE_8G  = 2u,
    QMI8658A_ACCEL_RANGE_16G = 3u
} qmi8658a_accel_range_t;

typedef enum {
    QMI8658A_GYRO_RANGE_16DPS   = 0u,
    QMI8658A_GYRO_RANGE_32DPS   = 1u,
    QMI8658A_GYRO_RANGE_64DPS   = 2u,
    QMI8658A_GYRO_RANGE_128DPS  = 3u,
    QMI8658A_GYRO_RANGE_256DPS  = 4u,
    QMI8658A_GYRO_RANGE_512DPS  = 5u,
    QMI8658A_GYRO_RANGE_1024DPS = 6u,
    QMI8658A_GYRO_RANGE_2048DPS = 7u
} qmi8658a_gyro_range_t;

typedef struct {
    qmi8658a_i2c_write_t        write;
    qmi8658a_i2c_read_t         read;
    qmi8658a_i2c_write_read_t   write_read;
    qmi8658a_delay_ms_t         delay_ms;
    uint8_t                     address;
    qmi8658a_accel_range_t      accel_range;
    qmi8658a_gyro_range_t       gyro_range;
} qmi8658a_config_t;

typedef struct {
    qmi8658a_i2c_write_t        write;
    qmi8658a_i2c_read_t         read;
    qmi8658a_i2c_write_read_t   write_read;
    qmi8658a_delay_ms_t         delay_ms;
    uint8_t                     address;
    qmi8658a_accel_range_t      accel_range;
    qmi8658a_gyro_range_t       gyro_range;
    bool                        initialized;
} qmi8658a_t;

qmi8658a_status_t qmi8658a_init_with_config(qmi8658a_t *dev, const qmi8658a_config_t *cfg);

qmi8658a_status_t qmi8658a_init(qmi8658a_t *dev,
                                uint8_t address,
                                qmi8658a_i2c_write_t write,
                                qmi8658a_i2c_read_t read,
                                qmi8658a_delay_ms_t delay_ms);

qmi8658a_status_t qmi8658a_set_accel_range(qmi8658a_t *dev, qmi8658a_accel_range_t range);
qmi8658a_status_t qmi8658a_set_gyro_range(qmi8658a_t *dev, qmi8658a_gyro_range_t range);

qmi8658a_status_t qmi8658a_read_raw(qmi8658a_t *dev,
                                    int16_t *ax,
                                    int16_t *ay,
                                    int16_t *az,
                                    int16_t *gx,
                                    int16_t *gy,
                                    int16_t *gz);

qmi8658a_status_t qmi8658a_read_temperature(qmi8658a_t *dev, float *temp_c);

#ifdef __cplusplus
}
#endif

#endif /* QMI8658A_H */
