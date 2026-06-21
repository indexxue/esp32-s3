/**
 * @file    ds3231.h
 * @brief   Generic DS3231 precision RTC (I2C + optional delay callbacks, no MCU dependencies).
 *
 * Typical 7-bit I2C address: 0x68 (A0/A1/A2 tied to GND).
 * Time registers use BCD; weekday 1..7 (Sunday = 1).
 */

#ifndef DS3231_H
#define DS3231_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 7-bit I2C slave address when A0/A1/A2 = 0. */
#define DS3231_I2C_ADDR_7BIT (0x68u)

typedef int (*ds3231_i2c_write_t)(uint8_t addr7, const uint8_t *data, uint16_t len);
typedef int (*ds3231_i2c_read_t)(uint8_t addr7, uint8_t *data, uint16_t len);
/** 可选：一次事务内先写寄存器地址再读（repeated start）。非 NULL 时读寄存器优先走此路径。 */
typedef int (*ds3231_i2c_write_read_t)(uint8_t addr7,
                                       const uint8_t *write_data,
                                       uint16_t write_len,
                                       uint8_t *read_data,
                                       uint16_t read_len);
typedef void (*ds3231_delay_ms_t)(uint32_t ms);

typedef enum {
    DS3231_OK = 0,
    DS3231_ERROR_I2C,
    DS3231_ERROR_NOT_INIT,
    DS3231_ERROR_PARAM,
    DS3231_ERROR_RANGE,
    DS3231_ERROR_BUSY,
    DS3231_ERROR_OSF
} ds3231_status_t;

typedef struct {
    uint16_t year;    /**< 完整年份，如 2026 */
    uint8_t month;    /**< 1..12 */
    uint8_t day;      /**< 1..31 */
    uint8_t weekday;  /**< 1..7，DS3231 约定 Sunday=1 */
    uint8_t hour;     /**< 0..23（24 小时制） */
    uint8_t minute;   /**< 0..59 */
    uint8_t second;   /**< 0..59 */
} ds3231_datetime_t;

typedef struct {
    bool oscillator_stop; /**< STATUS.OSF：曾掉电或振荡器停振 */
    bool temp_busy;       /**< STATUS.BSY：温度转换进行中 */
    bool alarm1_flag;
    bool alarm2_flag;
    bool en32khz;
} ds3231_status_flags_t;

typedef struct {
    ds3231_i2c_write_t      write;
    ds3231_i2c_read_t       read;
    ds3231_i2c_write_read_t write_read;
    ds3231_delay_ms_t       delay_ms;
    uint8_t                 address;
} ds3231_config_t;

typedef struct {
    ds3231_i2c_write_t      write;
    ds3231_i2c_read_t       read;
    ds3231_i2c_write_read_t write_read;
    ds3231_delay_ms_t       delay_ms;
    uint8_t                 address;
    bool                    initialized;
} ds3231_t;

ds3231_status_t ds3231_init_with_config(ds3231_t *dev, const ds3231_config_t *cfg);

ds3231_status_t ds3231_init(ds3231_t *dev,
                            uint8_t address,
                            ds3231_i2c_write_t write,
                            ds3231_i2c_read_t read,
                            ds3231_i2c_write_read_t write_read,
                            ds3231_delay_ms_t delay_ms);

/** 读秒寄存器，用于探测总线上是否存在 DS3231。 */
ds3231_status_t ds3231_probe(ds3231_t *dev);

ds3231_status_t ds3231_read_datetime(ds3231_t *dev, ds3231_datetime_t *dt);
ds3231_status_t ds3231_write_datetime(ds3231_t *dev, const ds3231_datetime_t *dt);

ds3231_status_t ds3231_read_status(ds3231_t *dev, ds3231_status_flags_t *flags);
ds3231_status_t ds3231_clear_osf(ds3231_t *dev);

/** 启动一次温度转换并等待完成（需 delay_ms 非 NULL）。 */
ds3231_status_t ds3231_force_temp_conversion(ds3231_t *dev);
ds3231_status_t ds3231_read_temperature(ds3231_t *dev, float *temp_c);

/** 停止/恢复 RTC 振荡器（秒寄存器 CH 位）。 */
ds3231_status_t ds3231_halt(ds3231_t *dev);
ds3231_status_t ds3231_start(ds3231_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* DS3231_H */
