/**
 * @file    ds3231.c
 * @brief   Generic DS3231 precision RTC (I2C + optional delay callbacks, no MCU dependencies).
 */

#include "ds3231.h"

#include <stddef.h>

#define DS3231_REG_SECONDS   0x00u
#define DS3231_REG_MINUTES   0x01u
#define DS3231_REG_HOURS     0x02u
#define DS3231_REG_DAY       0x03u
#define DS3231_REG_DATE      0x04u
#define DS3231_REG_MONTH     0x05u
#define DS3231_REG_YEAR      0x06u
#define DS3231_REG_CONTROL   0x0Eu
#define DS3231_REG_STATUS    0x0Fu
#define DS3231_REG_TEMP_MSB  0x11u

#define DS3231_SECONDS_CH    (1u << 7)
#define DS3231_MONTH_CENTURY (1u << 7)

#define DS3231_CTRL_EOSC     (1u << 7)
#define DS3231_CTRL_CONV     (1u << 5)

#define DS3231_STAT_OSF      (1u << 7)
#define DS3231_STAT_EN32KHZ  (1u << 3)
#define DS3231_STAT_BSY      (1u << 2)
#define DS3231_STAT_A2F      (1u << 1)
#define DS3231_STAT_A1F      (1u << 0)

#define DS3231_TEMP_CONV_TIMEOUT_MS (750u)
#define DS3231_TEMP_CONV_POLL_MS    (10u)

static uint8_t ds3231_bcd_to_dec(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10u) + (bcd & 0x0Fu));
}

static uint8_t ds3231_dec_to_bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10u) << 4) | (dec % 10u));
}

static bool ds3231_datetime_valid(const ds3231_datetime_t *dt)
{
    if (dt == NULL) {
        return false;
    }
    if (dt->year < 2000u || dt->year > 2099u) {
        return false;
    }
    if (dt->month < 1u || dt->month > 12u) {
        return false;
    }
    if (dt->day < 1u || dt->day > 31u) {
        return false;
    }
    if (dt->weekday < 1u || dt->weekday > 7u) {
        return false;
    }
    if (dt->hour > 23u || dt->minute > 59u || dt->second > 59u) {
        return false;
    }
    return true;
}

static ds3231_status_t ds3231_write_reg(ds3231_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t buf[2];

    if (dev == NULL || !dev->initialized || dev->write == NULL) {
        return DS3231_ERROR_NOT_INIT;
    }

    buf[0] = reg;
    buf[1] = value;
    if (dev->write(dev->address, buf, 2u) != 0) {
        return DS3231_ERROR_I2C;
    }
    return DS3231_OK;
}

static ds3231_status_t ds3231_read_regs(ds3231_t *dev, uint8_t reg, uint8_t *data, uint16_t len)
{
    if (dev == NULL || !dev->initialized || data == NULL || len == 0u) {
        return DS3231_ERROR_PARAM;
    }

    if (dev->write_read != NULL) {
        if (dev->write_read(dev->address, &reg, 1u, data, len) != 0) {
            return DS3231_ERROR_I2C;
        }
        return DS3231_OK;
    }

    if (dev->write == NULL || dev->read == NULL) {
        return DS3231_ERROR_PARAM;
    }

    if (dev->write(dev->address, &reg, 1u) != 0) {
        return DS3231_ERROR_I2C;
    }
    if (dev->read(dev->address, data, len) != 0) {
        return DS3231_ERROR_I2C;
    }
    return DS3231_OK;
}

static ds3231_status_t ds3231_read_reg(ds3231_t *dev, uint8_t reg, uint8_t *value)
{
    if (value == NULL) {
        return DS3231_ERROR_PARAM;
    }
    return ds3231_read_regs(dev, reg, value, 1u);
}

static ds3231_status_t ds3231_write_regs(ds3231_t *dev, uint8_t reg, const uint8_t *data, uint16_t len)
{
    uint8_t stack_buf[8];
    uint8_t *buf = stack_buf;

    if (dev == NULL || !dev->initialized || data == NULL || len == 0u) {
        return DS3231_ERROR_PARAM;
    }
    if (dev->write == NULL) {
        return DS3231_ERROR_NOT_INIT;
    }
    if (len > (uint16_t)(sizeof(stack_buf) - 1u)) {
        return DS3231_ERROR_PARAM;
    }

    buf[0] = reg;
    for (uint16_t i = 0; i < len; i++) {
        buf[i + 1u] = data[i];
    }
    if (dev->write(dev->address, buf, (uint16_t)(len + 1u)) != 0) {
        return DS3231_ERROR_I2C;
    }
    return DS3231_OK;
}

static ds3231_status_t ds3231_update_reg(ds3231_t *dev, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t cur = 0u;
    ds3231_status_t st;

    st = ds3231_read_reg(dev, reg, &cur);
    if (st != DS3231_OK) {
        return st;
    }
    cur = (uint8_t)((cur & (uint8_t)(~mask)) | (value & mask));
    return ds3231_write_reg(dev, reg, cur);
}

ds3231_status_t ds3231_init_with_config(ds3231_t *dev, const ds3231_config_t *cfg)
{
    uint8_t ctrl = 0u;

    if (dev == NULL || cfg == NULL || cfg->write == NULL) {
        return DS3231_ERROR_PARAM;
    }
    if ((cfg->write_read == NULL) && (cfg->read == NULL)) {
        return DS3231_ERROR_PARAM;
    }

    dev->write       = cfg->write;
    dev->read        = cfg->read;
    dev->write_read  = cfg->write_read;
    dev->delay_ms    = cfg->delay_ms;
    dev->address     = cfg->address;
    dev->initialized = true;

    /* 确保振荡器运行；关闭 1Hz 方波输出以免额外负载。 */
    if (ds3231_read_reg(dev, DS3231_REG_CONTROL, &ctrl) != DS3231_OK) {
        dev->initialized = false;
        return DS3231_ERROR_I2C;
    }
    ctrl = (uint8_t)(ctrl & (uint8_t)(~DS3231_CTRL_EOSC));
    ctrl = (uint8_t)(ctrl & 0xFCu);
    if (ds3231_write_reg(dev, DS3231_REG_CONTROL, ctrl) != DS3231_OK) {
        dev->initialized = false;
        return DS3231_ERROR_I2C;
    }

    return DS3231_OK;
}

ds3231_status_t ds3231_init(ds3231_t *dev,
                            uint8_t address,
                            ds3231_i2c_write_t write,
                            ds3231_i2c_read_t read,
                            ds3231_i2c_write_read_t write_read,
                            ds3231_delay_ms_t delay_ms)
{
    ds3231_config_t cfg = {
        .write       = write,
        .read        = read,
        .write_read  = write_read,
        .delay_ms    = delay_ms,
        .address     = address,
    };

    return ds3231_init_with_config(dev, &cfg);
}

ds3231_status_t ds3231_probe(ds3231_t *dev)
{
    uint8_t sec = 0u;

    if (dev == NULL || !dev->initialized) {
        return DS3231_ERROR_NOT_INIT;
    }
    return ds3231_read_reg(dev, DS3231_REG_SECONDS, &sec);
}

ds3231_status_t ds3231_read_datetime(ds3231_t *dev, ds3231_datetime_t *dt)
{
    uint8_t raw[7];
    ds3231_status_t st;
    uint8_t month_reg;

    if (dt == NULL) {
        return DS3231_ERROR_PARAM;
    }

    st = ds3231_read_regs(dev, DS3231_REG_SECONDS, raw, sizeof(raw));
    if (st != DS3231_OK) {
        return st;
    }

    dt->second  = ds3231_bcd_to_dec((uint8_t)(raw[0] & 0x7Fu));
    dt->minute  = ds3231_bcd_to_dec((uint8_t)(raw[1] & 0x7Fu));
    dt->hour    = ds3231_bcd_to_dec((uint8_t)(raw[2] & 0x3Fu));
    dt->weekday = ds3231_bcd_to_dec((uint8_t)(raw[3] & 0x07u));
    dt->day     = ds3231_bcd_to_dec((uint8_t)(raw[4] & 0x3Fu));
    month_reg   = (uint8_t)(raw[5] & 0x1Fu);
    dt->month   = ds3231_bcd_to_dec(month_reg);
    dt->year    = (uint16_t)(2000u + ds3231_bcd_to_dec(raw[6]));
    if ((raw[5] & DS3231_MONTH_CENTURY) != 0u) {
        dt->year = (uint16_t)(dt->year + 100u);
    }

    return DS3231_OK;
}

ds3231_status_t ds3231_write_datetime(ds3231_t *dev, const ds3231_datetime_t *dt)
{
    uint8_t raw[7];
    uint16_t year2;

    if (!ds3231_datetime_valid(dt)) {
        return DS3231_ERROR_RANGE;
    }

    year2 = (uint16_t)(dt->year % 100u);
    raw[0] = ds3231_dec_to_bcd(dt->second);
    raw[1] = ds3231_dec_to_bcd(dt->minute);
    raw[2] = ds3231_dec_to_bcd(dt->hour);
    raw[3] = ds3231_dec_to_bcd(dt->weekday);
    raw[4] = ds3231_dec_to_bcd(dt->day);
    raw[5] = ds3231_dec_to_bcd(dt->month);
    if (dt->year >= 2100u) {
        raw[5] = (uint8_t)(raw[5] | DS3231_MONTH_CENTURY);
    }
    raw[6] = ds3231_dec_to_bcd((uint8_t)year2);

    if (ds3231_write_regs(dev, DS3231_REG_SECONDS, raw, sizeof(raw)) != DS3231_OK) {
        return DS3231_ERROR_I2C;
    }

    return ds3231_clear_osf(dev);
}

ds3231_status_t ds3231_read_status(ds3231_t *dev, ds3231_status_flags_t *flags)
{
    uint8_t stat = 0u;
    ds3231_status_t st;

    if (flags == NULL) {
        return DS3231_ERROR_PARAM;
    }

    st = ds3231_read_reg(dev, DS3231_REG_STATUS, &stat);
    if (st != DS3231_OK) {
        return st;
    }

    flags->oscillator_stop = ((stat & DS3231_STAT_OSF) != 0u);
    flags->temp_busy       = ((stat & DS3231_STAT_BSY) != 0u);
    flags->alarm1_flag     = ((stat & DS3231_STAT_A1F) != 0u);
    flags->alarm2_flag     = ((stat & DS3231_STAT_A2F) != 0u);
    flags->en32khz         = ((stat & DS3231_STAT_EN32KHZ) != 0u);
    return DS3231_OK;
}

ds3231_status_t ds3231_clear_osf(ds3231_t *dev)
{
    return ds3231_update_reg(dev, DS3231_REG_STATUS, DS3231_STAT_OSF, 0u);
}

ds3231_status_t ds3231_force_temp_conversion(ds3231_t *dev)
{
    ds3231_status_t st;
    uint32_t waited_ms = 0u;
    uint8_t stat = 0u;

    if (dev == NULL || !dev->initialized) {
        return DS3231_ERROR_NOT_INIT;
    }
    if (dev->delay_ms == NULL) {
        return DS3231_ERROR_PARAM;
    }

    st = ds3231_update_reg(dev, DS3231_REG_CONTROL, DS3231_CTRL_CONV, DS3231_CTRL_CONV);
    if (st != DS3231_OK) {
        return st;
    }

    while (waited_ms < DS3231_TEMP_CONV_TIMEOUT_MS) {
        st = ds3231_read_reg(dev, DS3231_REG_STATUS, &stat);
        if (st != DS3231_OK) {
            return st;
        }
        if ((stat & DS3231_STAT_BSY) == 0u) {
            return DS3231_OK;
        }
        dev->delay_ms(DS3231_TEMP_CONV_POLL_MS);
        waited_ms += DS3231_TEMP_CONV_POLL_MS;
    }

    return DS3231_ERROR_BUSY;
}

ds3231_status_t ds3231_read_temperature(ds3231_t *dev, float *temp_c)
{
    uint8_t raw[2];
    ds3231_status_t st;
    int8_t integer_part;

    if (temp_c == NULL) {
        return DS3231_ERROR_PARAM;
    }

    st = ds3231_read_regs(dev, DS3231_REG_TEMP_MSB, raw, sizeof(raw));
    if (st != DS3231_OK) {
        return st;
    }

    integer_part = (int8_t)raw[0];
    *temp_c      = (float)integer_part + (float)((raw[1] >> 6) & 0x03u) * 0.25f;
    return DS3231_OK;
}

ds3231_status_t ds3231_halt(ds3231_t *dev)
{
    return ds3231_update_reg(dev, DS3231_REG_SECONDS, DS3231_SECONDS_CH, DS3231_SECONDS_CH);
}

ds3231_status_t ds3231_start(ds3231_t *dev)
{
    return ds3231_update_reg(dev, DS3231_REG_SECONDS, DS3231_SECONDS_CH, 0u);
}
