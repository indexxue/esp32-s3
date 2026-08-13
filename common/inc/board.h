#ifndef COMMON_BOARD_H
#define COMMON_BOARD_H

#include "type.h"

#include "device_profile.h"
#include "ds3231.h"
#include "qmi8658a.h"
#include "spi.h"
#include "st7789.h"
#include "gc9a01.h"
#include "adc.h"

#if defined(BOARD_PROFILE_VOICE_HUB)

/* -------------------------------------------------------------------------- */
/* voice_hub_rev_a — 240×135 ST7789 / OV2640 / 1-bit SDMMC                           */
/* 仅一条 I2C（GPIO4/5）：OV2640 SCCB；无其它 I2C 外设。                          */
/* -------------------------------------------------------------------------- */

#define BOARD_GPIO_IO5 (-1)
#define BOARD_GPIO_IO4_PWM (-1)

#define BOARD_I2C_BUS1_HW_PORT (0)
/** voice_hub_rev_a：SCL=GPIO4，SDA=GPIO5（OV2640 SCCB）。 */
#define BOARD_I2C_BUS1_PIN_SCL (4)
#define BOARD_I2C_BUS1_PIN_SDA (5)

#define BOARD_I2C_DEFAULT_CLOCK_HZ (100000U)
#define BOARD_I2C_DEFAULT_TIMEOUT_MS (200U)
#define BOARD_I2C_MAX_DEVICES (8U)
#define BOARD_I2C_GLITCH_IGNORE (7U)

/** voice_hub：OV2640 需 XCLK 后 SCCB 才应答，扫描改在 camera 模块内（XCLK 前后各一次）。 */
#ifndef BOARD_I2C_BUS1_SCAN_ON_BOOT
#define BOARD_I2C_BUS1_SCAN_ON_BOOT (0)
#endif

/** OV2640 SCCB 使用 I2C1。 */
#define BOARD_I2C_OV2640_SCCB_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_I2C_OV2640_SCCB_ADDR (0x30U)

/** board.c 编译占位；voice_hub 无 I2C2 / IMU / RTC。 */
#define BOARD_I2C_BUS2_HW_PORT (1)
#define BOARD_I2C_BUS2_PIN_SCL (-1)
#define BOARD_I2C_BUS2_PIN_SDA (-1)
#define BOARD_I2C_QMI8658A_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_I2C_QMI8658A_ADDR (0x6AU)
#define BOARD_I2C_DS3231_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_I2C_DS3231_ADDR DS3231_I2C_ADDR_7BIT
#ifndef BOARD_DS3231_SYNC_TIME_ON_BOOT
#define BOARD_DS3231_SYNC_TIME_ON_BOOT (0)
#endif
#define BOARD_DS3231_SYNC_YEAR (2026U)
#define BOARD_DS3231_SYNC_MONTH (6U)
#define BOARD_DS3231_SYNC_DAY (21U)
#define BOARD_DS3231_SYNC_WEEKDAY (1U)
#define BOARD_DS3231_SYNC_HOUR (0U)
#define BOARD_DS3231_SYNC_MINUTE (0U)
#define BOARD_DS3231_SYNC_SECOND (0U)

#define BOARD_ST7789_SPI_HOST (SPI_HOST_2_E)
#define BOARD_ST7789_PIN_SCK (38)
#define BOARD_ST7789_PIN_MOSI (42)
#define BOARD_ST7789_PIN_CS (39)
#define BOARD_ST7789_PIN_DC (41)
#define BOARD_ST7789_PIN_RST (40)
#define BOARD_ST7789_PIN_BL (45)
#define BOARD_ST7789_SPI_DEV_CS_PIN (-1)
#define BOARD_ST7789_SPI_MAX_TX (32768)
#define BOARD_ST7789_SPI_CLOCK_HZ (40000000U)

#define BOARD_OV2640_PIN_XCLK (15)
#define BOARD_OV2640_PIN_PCLK (13)
#define BOARD_OV2640_PIN_VSYNC (6)
#define BOARD_OV2640_PIN_HREF (7)
#define BOARD_OV2640_PIN_D0 (11)
#define BOARD_OV2640_PIN_D1 (9)
#define BOARD_OV2640_PIN_D2 (8)
#define BOARD_OV2640_PIN_D3 (10)
#define BOARD_OV2640_PIN_D4 (12)
#define BOARD_OV2640_PIN_D5 (18)
#define BOARD_OV2640_PIN_D6 (17)
#define BOARD_OV2640_PIN_D7 (16)
#define BOARD_OV2640_PIN_PWDN (-1)
#define BOARD_OV2640_PIN_RESET (-1)
#define BOARD_OV2640_XCLK_HZ (20000000U)

#define BOARD_SDCARD_PIN_CMD (21)
#define BOARD_SDCARD_PIN_CLK (14)
#define BOARD_SDCARD_PIN_D0 (2)
#define BOARD_SDCARD_PIN_D1 (-1)
#define BOARD_SDCARD_PIN_D2 (-1)
#define BOARD_SDCARD_PIN_D3 (-1)
#define BOARD_SDCARD_BUS_WIDTH (1U)
/** 1-bit + GPIO 矩阵：先用 10MHz，稳定后可试 20000。IDF 默认 CMD=15，本板 CMD=21。 */
#define BOARD_SDCARD_MAX_FREQ_KHZ (10000U)
#define BOARD_SDCARD_SDMMC_DMA_PATH (0U)
#define BOARD_SDCARD_HOST_FLAGS_EXTRA (0U)
#define BOARD_SDCARD_MOUNT_POINT "/sdcard"

/** N16R8 Octal PSRAM 占用 GPIO35–37，按键须避开；与 project 一致用 GPIO0。 */
#define BOARD_VOICE_HUB_PIN_BUTTON (0)
#define BOARD_VOICE_HUB_WS2812_PIN (48)
#define BOARD_VOICE_HUB_WS2812_COUNT (1U)

/** M1→M3 bring-up：填好本段引脚后按需置 1。 */
#ifndef VOICE_HUB_ENABLE_LCD
#define VOICE_HUB_ENABLE_LCD 1
#endif
#ifndef VOICE_HUB_ENABLE_WIFI_WEB
#define VOICE_HUB_ENABLE_WIFI_WEB 1
#endif
#ifndef VOICE_HUB_ENABLE_CAMERA
#define VOICE_HUB_ENABLE_CAMERA 1
#endif
#ifndef VOICE_HUB_ENABLE_SDCARD
#define VOICE_HUB_ENABLE_SDCARD 0
#endif

/** 电池分压 ADC：GPIO3（ADC1_CH2）；无独立采样使能脚。 */
#define BOARD_BATTERY_PIN_ENABLE (-1)
#define BOARD_BATTERY_PIN_ADC (3)
#define BOARD_BATTERY_ADC_CHANNEL ADC_CHANNEL_2_E

#define BOARD_IR_SENSOR0_PIN (-1)
#define BOARD_IR_SENSOR1_PIN (-1)
#define BOARD_IR_SENSOR_COUNT (2U)
#define BOARD_IR_DEBOUNCE_MS (400U)

#define BOARD_BUZZER_PIN (-1)
#define BOARD_BUZZER_ACTIVE_LEVEL (1U)

#define BOARD_IO4_PWM_FREQ_HZ (5000U)

#elif defined(BOARD_PROFILE_CAMERA)

/* -------------------------------------------------------------------------- */
/* camera_rev_a — OV2640 / SoftAP web / dual servo / host SPI / QMI8658A      */
/* I2C1 GPIO10/11：OV2640 SCCB；I2C2 GPIO14/13：QMI8658A。无 LCD。              */
/* -------------------------------------------------------------------------- */

#define BOARD_GPIO_IO5 (-1)
#define BOARD_GPIO_IO4_PWM (-1)

#define BOARD_I2C_BUS1_HW_PORT (0)
#define BOARD_I2C_BUS1_PIN_SCL (10)
#define BOARD_I2C_BUS1_PIN_SDA (11)

#define BOARD_I2C_DEFAULT_CLOCK_HZ (100000U)
#define BOARD_I2C_DEFAULT_TIMEOUT_MS (200U)
#define BOARD_I2C_MAX_DEVICES (8U)
#define BOARD_I2C_GLITCH_IGNORE (7U)

#ifndef BOARD_I2C_BUS1_SCAN_ON_BOOT
#define BOARD_I2C_BUS1_SCAN_ON_BOOT (0)
#endif

#define BOARD_I2C_OV2640_SCCB_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_I2C_OV2640_SCCB_ADDR (0x30U)

#define BOARD_I2C_BUS2_HW_PORT (1)
#define BOARD_I2C_BUS2_PIN_SCL (14)
#define BOARD_I2C_BUS2_PIN_SDA (13)
#define BOARD_I2C_QMI8658A_PORT BOARD_I2C_BUS2_HW_PORT
#define BOARD_I2C_QMI8658A_ADDR (0x6AU)
#define BOARD_I2C_DS3231_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_I2C_DS3231_ADDR DS3231_I2C_ADDR_7BIT
#ifndef BOARD_DS3231_SYNC_TIME_ON_BOOT
#define BOARD_DS3231_SYNC_TIME_ON_BOOT (0)
#endif
#define BOARD_DS3231_SYNC_YEAR (2026U)
#define BOARD_DS3231_SYNC_MONTH (6U)
#define BOARD_DS3231_SYNC_DAY (21U)
#define BOARD_DS3231_SYNC_WEEKDAY (1U)
#define BOARD_DS3231_SYNC_HOUR (0U)
#define BOARD_DS3231_SYNC_MINUTE (0U)
#define BOARD_DS3231_SYNC_SECOND (0U)

/* camera 无 LCD：宏仅供 board.c 编译占位，勿接外设。 */
#define BOARD_ST7789_SPI_HOST (SPI_HOST_2_E)
#define BOARD_ST7789_PIN_SCK (-1)
#define BOARD_ST7789_PIN_MOSI (-1)
#define BOARD_ST7789_PIN_CS (-1)
#define BOARD_ST7789_PIN_DC (-1)
#define BOARD_ST7789_PIN_RST (-1)
#define BOARD_ST7789_PIN_BL (-1)
#define BOARD_ST7789_SPI_DEV_CS_PIN (-1)
#define BOARD_ST7789_SPI_MAX_TX (32768)
#define BOARD_ST7789_SPI_CLOCK_HZ (40000000U)

#define BOARD_OV2640_PIN_XCLK (8)
#define BOARD_OV2640_PIN_PCLK (16)
#define BOARD_OV2640_PIN_VSYNC (9)
#define BOARD_OV2640_PIN_HREF (46)
#define BOARD_OV2640_PIN_D0 (7)
#define BOARD_OV2640_PIN_D1 (5)
#define BOARD_OV2640_PIN_D2 (4)
#define BOARD_OV2640_PIN_D3 (6)
#define BOARD_OV2640_PIN_D4 (15)
#define BOARD_OV2640_PIN_D5 (17)
#define BOARD_OV2640_PIN_D6 (18)
#define BOARD_OV2640_PIN_D7 (3)
#define BOARD_OV2640_PIN_PWDN (-1)
#define BOARD_OV2640_PIN_RESET (12)
#define BOARD_OV2640_XCLK_HZ (20000000U)

/* camera: no SD card slot (macros only for sdcard.c compilation) */
#define BOARD_SDCARD_PIN_CMD (-1)
#define BOARD_SDCARD_PIN_CLK (-1)
#define BOARD_SDCARD_PIN_D0 (-1)
#define BOARD_SDCARD_PIN_D1 (-1)
#define BOARD_SDCARD_PIN_D2 (-1)
#define BOARD_SDCARD_PIN_D3 (-1)
#define BOARD_SDCARD_BUS_WIDTH (1U)
#define BOARD_SDCARD_MAX_FREQ_KHZ (0U)
#define BOARD_SDCARD_SDMMC_DMA_PATH (0U)
#define BOARD_SDCARD_HOST_FLAGS_EXTRA (0U)
#define BOARD_SDCARD_MOUNT_POINT "/sdcard"

#define BOARD_CAMERA_PIN_BUTTON (45) /* BTN1 */
#define BOARD_CAMERA_WS2812_PIN (48)
#define BOARD_CAMERA_WS2812_COUNT (4U)

/** GPIO1：12V 电源开关继电器（高开 / 低关，按硬件确认）。 */
#define BOARD_CAMERA_PIN_PWR_RELAY (1)
#define BOARD_CAMERA_PWR_RELAY_ACTIVE_LEVEL (1U)

/** 本板无 LCD；保留开关供 start.c 条件编译，固定为 0。 */
#ifndef CAMERA_ENABLE_LCD
#define CAMERA_ENABLE_LCD 0
#endif

/*
 * MG996R 双舵机（LEDC 50 Hz）。
 * 画面坐标：原点左上，x 右、y 下。
 *   Pan  (PWM1/GPIO21)：err_x>0 → 右转（使目标回中）；反向时翻 BOARD_SERVO_PAN_SIGN
 *   Tilt (PWM2/GPIO47)：err_y>0 → 下俯；反向时翻 BOARD_SERVO_TILT_SIGN
 * 角度 0–ANGLE_MAX 线性映射 PULSE_MIN–MAX µs；中位 ANGLE_MAX/2 → CENTER_PULSE。
 * 软限位为上电默认（Pan 全行程、Tilt 240° 窗），运行时可由 /api/servo 改。
 */
#define BOARD_SERVO_PAN_PIN (21)
#define BOARD_SERVO_TILT_PIN (47)
#define BOARD_SERVO_PWM_FREQ_HZ (50U)
#define BOARD_SERVO_PWM_TIMER (0)       /* PWM_TIMER_0_E */
#define BOARD_SERVO_PAN_PWM_CH (0)      /* PWM_CHANNEL_0_E */
#define BOARD_SERVO_TILT_PWM_CH (1)     /* PWM_CHANNEL_1_E */
#define BOARD_SERVO_PULSE_MIN_US (500U)
#define BOARD_SERVO_PULSE_MAX_US (2500U)
#define BOARD_SERVO_CENTER_PULSE_US (1500U)
#define BOARD_SERVO_ANGLE_MAX_DEG (360)
#define BOARD_SERVO_CENTER_DEG (180)
#define BOARD_SERVO_PAN_MIN_DEG (0)
#define BOARD_SERVO_PAN_MAX_DEG (360)
/* Tilt 机械行程约 240°：以中位对称默认窗 60–300 */
#define BOARD_SERVO_TILT_MIN_DEG (60)
#define BOARD_SERVO_TILT_MAX_DEG (300)
#define BOARD_SERVO_PAN_SIGN (1)
#define BOARD_SERVO_TILT_SIGN (1)

/*
 * 对外通信 SPI1（产品名）→ 硬件 SPI3。
 * 锁定：Mode1 (CPOL=0,CPHA=1) / 1 MHz / 32B / ~20 ms。
 * SCK=39 MOSI=40 MISO=41 CS=38。
 */
#define BOARD_SPI1_HOST (SPI_HOST_3_E)
#define BOARD_SPI1_PIN_SCK (39)
#define BOARD_SPI1_PIN_MOSI (40)
#define BOARD_SPI1_PIN_MISO (41)
#define BOARD_SPI1_PIN_CS (38)
#ifndef BOARD_SPI1_CLOCK_HZ
#define BOARD_SPI1_CLOCK_HZ (1000000U)
#endif
#ifndef BOARD_SPI1_POLL_MS
#define BOARD_SPI1_POLL_MS (20U)
#endif
#ifndef BOARD_SPI1_ENABLE
#define BOARD_SPI1_ENABLE (1)
#endif

/* camera: no battery ADC */
#define BOARD_BATTERY_PIN_ENABLE (-1)
#define BOARD_BATTERY_PIN_ADC (-1)
#define BOARD_BATTERY_ADC_CHANNEL ADC_CHANNEL_0_E

#define BOARD_IR_SENSOR0_PIN (-1)
#define BOARD_IR_SENSOR1_PIN (-1)
#define BOARD_IR_SENSOR_COUNT (2U)
#define BOARD_IR_DEBOUNCE_MS (400U)

#define BOARD_BUZZER_PIN (-1)
#define BOARD_BUZZER_ACTIVE_LEVEL (1U)

#define BOARD_IO4_PWM_FREQ_HZ (5000U)

#elif defined(BOARD_PROFILE_DESKTOP_PET)

/* -------------------------------------------------------------------------- */
/* desktop_pet_rev_a — 桌宠实板引脚（改接线只改本段）                            */
/* 单 I2C：SCL=18 SDA=8（ES8311 / QMI8685 / IT7259 同总线）。无触摸 INT/RST。      */
/* 圆屏 GC9A01 SPI；N16R8 勿占用 GPIO35–37。                                    */
/* -------------------------------------------------------------------------- */

#define BOARD_GPIO_IO5 (-1)
#define BOARD_GPIO_IO4_PWM (-1)

/** 唯一 I2C：SCL=GPIO18，SDA=GPIO8。 */
#define BOARD_I2C_BUS1_HW_PORT (0)
#define BOARD_I2C_BUS1_PIN_SCL (18)
#define BOARD_I2C_BUS1_PIN_SDA (8)

#define BOARD_I2C_DEFAULT_CLOCK_HZ (100000U)
#define BOARD_I2C_DEFAULT_TIMEOUT_MS (200U)
#define BOARD_I2C_MAX_DEVICES (8U)
#define BOARD_I2C_GLITCH_IGNORE (7U)

#ifndef BOARD_I2C_BUS1_SCAN_ON_BOOT
#define BOARD_I2C_BUS1_SCAN_ON_BOOT (1)
#endif

/** 本板无第二路 I2C。 */
#define BOARD_I2C_BUS2_HW_PORT (1)
#define BOARD_I2C_BUS2_PIN_SCL (-1)
#define BOARD_I2C_BUS2_PIN_SDA (-1)

/** QMI8685 挂 I2C1（驱动暂用 qmi8658a）。 */
#define BOARD_I2C_QMI8658A_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_I2C_QMI8658A_ADDR (0x6AU)

/** 本板无 RTC；占位供 board.c 编译。 */
#define BOARD_I2C_DS3231_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_I2C_DS3231_ADDR DS3231_I2C_ADDR_7BIT
#ifndef BOARD_DS3231_SYNC_TIME_ON_BOOT
#define BOARD_DS3231_SYNC_TIME_ON_BOOT (0)
#endif
#define BOARD_DS3231_SYNC_YEAR (2026U)
#define BOARD_DS3231_SYNC_MONTH (6U)
#define BOARD_DS3231_SYNC_DAY (21U)
#define BOARD_DS3231_SYNC_WEEKDAY (1U)
#define BOARD_DS3231_SYNC_HOUR (0U)
#define BOARD_DS3231_SYNC_MINUTE (0U)
#define BOARD_DS3231_SYNC_SECOND (0U)

/**
 * GC9A01 1.28" 圆屏 SPI（240×240）。
 * 无独立 LCD_RST：靠重新上电复位（PIN_RST=-1）。
 */
#define BOARD_GC9A01_SPI_HOST (SPI_HOST_2_E)
#define BOARD_GC9A01_PIN_SCK (7)
#define BOARD_GC9A01_PIN_MOSI (16)
#define BOARD_GC9A01_PIN_MISO (15)
#define BOARD_GC9A01_PIN_CS (6)
#define BOARD_GC9A01_PIN_DC (17)
#define BOARD_GC9A01_PIN_RST (-1)
#define BOARD_GC9A01_PIN_BL (5)
#define BOARD_GC9A01_SPI_DEV_CS_PIN (-1)
#define BOARD_GC9A01_SPI_MAX_TX (32768)
#define BOARD_GC9A01_SPI_CLOCK_HZ (40000000U)
/** cmd.c lcdbench 等共用时钟宏名时的别名。 */
#define BOARD_ST7789_SPI_CLOCK_HZ BOARD_GC9A01_SPI_CLOCK_HZ

#define BOARD_DESKTOP_PET_LCD_WIDTH GC9A01_PANEL_W
#define BOARD_DESKTOP_PET_LCD_HEIGHT GC9A01_PANEL_H

/** 电容触摸 IT7259：同 I2C1，7-bit 地址 0x46（8-bit 写 0x8C）；无 INT/RST。 */
#define BOARD_DESKTOP_PET_TOUCH_I2C_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_DESKTOP_PET_TOUCH_I2C_ADDR (0x46U)
#define BOARD_DESKTOP_PET_TOUCH_PIN_INT (-1)
#define BOARD_DESKTOP_PET_TOUCH_PIN_RST (-1)

/** ES8311：I2C1 控制 + I2S；SPE_EN=功放使能。丝印按板级命名。 */
#define BOARD_DESKTOP_PET_ES8311_I2C_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_DESKTOP_PET_ES8311_I2C_ADDR (0x18U)
#define BOARD_DESKTOP_PET_I2S_DIN_PIN (9)   /* I2S_DIN */
#define BOARD_DESKTOP_PET_I2S_WS_PIN (10)   /* I2S_LRCK */
#define BOARD_DESKTOP_PET_I2S_DOUT_PIN (11) /* I2S_DOUT */
#define BOARD_DESKTOP_PET_I2S_BCLK_PIN (12) /* I2S_BCLK */
#define BOARD_DESKTOP_PET_I2S_MCLK_PIN (13) /* I2S_MCLK */
#define BOARD_DESKTOP_PET_PA_EN_PIN (14)    /* SPE_EN */
#define BOARD_DESKTOP_PET_PA_EN_ACTIVE_LEVEL (1U)

/**
 * TB6612：M1=AIN + PWMA，M2=BIN + PWMB。
 * STBY=-1 表示板上常使能（或硬件上拉）。
 */
#define BOARD_DESKTOP_PET_TB6612_PIN_PWMA (45)
#define BOARD_DESKTOP_PET_TB6612_PIN_PWMB (38)
#define BOARD_DESKTOP_PET_TB6612_PIN_AIN1 (46) /* M1_IN1 */
#define BOARD_DESKTOP_PET_TB6612_PIN_AIN2 (3)  /* M1_IN2 */
#define BOARD_DESKTOP_PET_TB6612_PIN_BIN1 (21) /* M2_IN1 */
#define BOARD_DESKTOP_PET_TB6612_PIN_BIN2 (47) /* M2_IN2 */
#define BOARD_DESKTOP_PET_TB6612_PIN_STBY (-1)

/** SDMMC 4 线。bring-up 先 10MHz；稳定后再提速。 */
#define BOARD_SDCARD_PIN_CMD (41)
#define BOARD_SDCARD_PIN_CLK (42)
#define BOARD_SDCARD_PIN_D0 (2)
#define BOARD_SDCARD_PIN_D1 (1)
#define BOARD_SDCARD_PIN_D2 (39)
#define BOARD_SDCARD_PIN_D3 (40)
#define BOARD_SDCARD_BUS_WIDTH (4U)
#define BOARD_SDCARD_MAX_FREQ_KHZ (10000U)
#define BOARD_SDCARD_SDMMC_DMA_PATH (0U)
#define BOARD_SDCARD_HOST_FLAGS_EXTRA (0U)
#define BOARD_SDCARD_MOUNT_POINT "/sdcard"

#define BOARD_DESKTOP_PET_PIN_BUTTON (0)
#define BOARD_DESKTOP_PET_WS2812_PIN (48) /* RGB_DIN */
#define BOARD_DESKTOP_PET_WS2812_COUNT (4U)

/**
 * 锂电池 100K/100K 分压 → GPIO4（ADC1_CH3）：Vbat = 2 * Vadc。
 * 无独立采样使能脚（ENABLE=-1，分压常通）。
 */
#define BOARD_BATTERY_PIN_ENABLE (-1)
#define BOARD_BATTERY_PIN_ADC (4)
#define BOARD_BATTERY_ADC_CHANNEL ADC_CHANNEL_3_E

/** M1→… bring-up：填好引脚后按需置 1（改后重编）。 */
#ifndef DESKTOP_PET_ENABLE_LCD
#define DESKTOP_PET_ENABLE_LCD 1
#endif
#ifndef DESKTOP_PET_ENABLE_TOUCH
#define DESKTOP_PET_ENABLE_TOUCH 1
#endif
#ifndef DESKTOP_PET_ENABLE_IMU
#define DESKTOP_PET_ENABLE_IMU 1
#endif
#ifndef DESKTOP_PET_ENABLE_AUDIO
#define DESKTOP_PET_ENABLE_AUDIO 1
#endif
#ifndef DESKTOP_PET_ENABLE_MOTOR
#define DESKTOP_PET_ENABLE_MOTOR 1
#endif
#ifndef DESKTOP_PET_ENABLE_SDCARD
#define DESKTOP_PET_ENABLE_SDCARD 1
#endif
#ifndef DESKTOP_PET_ENABLE_WIFI_WEB
#define DESKTOP_PET_ENABLE_WIFI_WEB 1
#endif

#define BOARD_IR_SENSOR0_PIN (-1)
#define BOARD_IR_SENSOR1_PIN (-1)
#define BOARD_IR_SENSOR_COUNT (2U)
#define BOARD_IR_DEBOUNCE_MS (400U)

#define BOARD_BUZZER_PIN (-1)
#define BOARD_BUZZER_ACTIVE_LEVEL (1U)

#define BOARD_IO4_PWM_FREQ_HZ (5000U)

#else
#define BOARD_GPIO_IO5 (5)
#define BOARD_GPIO_IO4_PWM (4)

/**
 * 逻辑 I2C1 / I2C2 对应 ESP32-S3 硬件 I2C_NUM_0 / I2C_NUM_1。
 * I2C1：SCL=GPIO4，SDA=GPIO5；I2C2：SDA=GPIO17，SCL=GPIO18。
 */
#define BOARD_I2C_BUS1_HW_PORT (0)
#define BOARD_I2C_BUS2_HW_PORT (1)
#define BOARD_I2C_BUS1_PIN_SCL (4)
#define BOARD_I2C_BUS1_PIN_SDA (5)
#define BOARD_I2C_BUS2_PIN_SDA (17)
#define BOARD_I2C_BUS2_PIN_SCL (18)

#define BOARD_I2C_DEFAULT_CLOCK_HZ (100000U)
#define BOARD_I2C_DEFAULT_TIMEOUT_MS (200U)
#define BOARD_I2C_MAX_DEVICES (8U)
#define BOARD_I2C_GLITCH_IGNORE (7U)

/**
 * 上电后对 I²C1（GPIO4/5）做 7-bit 地址扫描 0x08..0x77 并打日志。
 * 量产可编译前 `#define BOARD_I2C_BUS1_SCAN_ON_BOOT 0` 关闭以缩短启动时间。
 */
#ifndef BOARD_I2C_BUS1_SCAN_ON_BOOT
#define BOARD_I2C_BUS1_SCAN_ON_BOOT (1)
#endif

/** QMI8658A：接在 I2C2 上，7-bit 地址 0x6A（SA0 接 GND 时常用）。 */
#define BOARD_I2C_QMI8658A_PORT BOARD_I2C_BUS2_HW_PORT
#define BOARD_I2C_QMI8658A_ADDR (0x6AU)

/** DS3231 RTC：接在 I2C1（GPIO4/5）上，7-bit 地址 0x68。 */
#define BOARD_I2C_DS3231_PORT BOARD_I2C_BUS1_HW_PORT
#define BOARD_I2C_DS3231_ADDR DS3231_I2C_ADDR_7BIT

/**
 * 上电是否将 DS3231 写入下方固定时间（联调用）。
 * 量产后请设为 0，改由 NTP / 菜单 / 外部工具校时。
 */
#ifndef BOARD_DS3231_SYNC_TIME_ON_BOOT
#define BOARD_DS3231_SYNC_TIME_ON_BOOT (0)
#endif
#define BOARD_DS3231_SYNC_YEAR    (2026U)
#define BOARD_DS3231_SYNC_MONTH   (6U)
#define BOARD_DS3231_SYNC_DAY     (21U)
#define BOARD_DS3231_SYNC_WEEKDAY (1U) /**< DS3231：1=Sunday … 7=Saturday */
#define BOARD_DS3231_SYNC_HOUR    (20U)
#define BOARD_DS3231_SYNC_MINUTE  (40U)
#define BOARD_DS3231_SYNC_SECOND  (0U)

/** ST7789：SPI2 与 TFT 控制脚（与硬件接线一致）。 */
#define BOARD_ST7789_SPI_HOST (SPI_HOST_2_E)
#define BOARD_ST7789_PIN_SCK (12)
#define BOARD_ST7789_PIN_MOSI (11)
#define BOARD_ST7789_PIN_CS (14)
#define BOARD_ST7789_PIN_DC (10)
#define BOARD_ST7789_PIN_RST (9)
#define BOARD_ST7789_PIN_BL (46)
/** `SpiTransmit` 槽位键：CS 由软件控制，SPI 设备使用 `GPIO_NUM_NC`。 */
#define BOARD_ST7789_SPI_DEV_CS_PIN (-1)
#define BOARD_ST7789_SPI_MAX_TX (32768)
/** SPI 时钟；全屏 DMA 基准可尝试 60000000U~80000000U 探硬件极限，花屏请降回 40MHz。 */
#define BOARD_ST7789_SPI_CLOCK_HZ (40000000U)

/**
 * ballot_guard 红外 proximity 传感器（OUT → MCU，输入上拉，靠近时对地，下降沿触发）。
 * 本产品无 SD 卡，GPIO38/39 专用于红外。
 */
#define BOARD_IR_SENSOR0_PIN (38)
#define BOARD_IR_SENSOR1_PIN (39)
#define BOARD_IR_SENSOR_COUNT (2U)
/** 同通道两次下降沿有效触发最短间隔（ms）；须小于产品冷却时长。 */
#define BOARD_IR_DEBOUNCE_MS (400U)

/** ballot_guard 有源蜂鸣器（GPIO 高电平响）。无源时改用 LEDC PWM 驱动同一引脚。 */
#define BOARD_BUZZER_PIN (47)
#define BOARD_BUZZER_ACTIVE_LEVEL (1U)

/**
 * SD 卡：SDMMC 4 线（main/project 等产品；ballot_guard 不使用）。
 * CMD=GPIO38, CLK=GPIO39, DAT0~DAT2=GPIO40~42, DAT3=GPIO2。
 */
#define BOARD_SDCARD_PIN_CMD (38)
#define BOARD_SDCARD_PIN_CLK (39)
#define BOARD_SDCARD_PIN_D0 (40)
#define BOARD_SDCARD_PIN_D1 (41)
#define BOARD_SDCARD_PIN_D2 (42)
#define BOARD_SDCARD_PIN_D3 (2)
#define BOARD_SDCARD_BUS_WIDTH (4U)
/**
 * 主机 SDMMC 时钟上限（kHz）。`0` 为默认速度档（通常 20MHz）；`40000` 为 SD High Speed 上限档（实际频率由卡与分频决定）。
 * 极限吞吐测试可设为 40000；信号差或花屏读写请改回 0 或 20000。
 */
#define BOARD_SDCARD_MAX_FREQ_KHZ (40000U)
/** 与 `DmaSdmmcPath_t` 一致，当前仅 `0`（`DMA_SDMMC_PATH_CONTROLLER_IDMAC_E`，主机内置 IDMAC）。 */
#define BOARD_SDCARD_SDMMC_DMA_PATH (0U)
/** 追加到 `sdmmc_host_t.flags`，如 `SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF`（见 IDF `sd_protocol_types.h`）。 */
#define BOARD_SDCARD_HOST_FLAGS_EXTRA (0U)
#define BOARD_SDCARD_MOUNT_POINT "/sdcard"

/**
 * 锂电池分压采样：GPIO6 高电平使能分压/采样电路；GPIO1 接 ADC（ESP32-S3 ADC1_CH0）。
 * 默认与 STM32 工程一致为 100K+100K 分压时，电池电压 Vbat = 2 * Vadc。
 */
#define BOARD_BATTERY_PIN_ENABLE (6)
#define BOARD_BATTERY_PIN_ADC (1)
#ifndef BOARD_BATTERY_ADC_CHANNEL
#define BOARD_BATTERY_ADC_CHANNEL ADC_CHANNEL_0_E
#endif

/** IO4 呼吸灯 PWM 频率（与 BoardInit 中配置一致） */
#define BOARD_IO4_PWM_FREQ_HZ (5000U)

#endif /* BOARD_PROFILE_* */

/**
 * 板级外设：按 device_profile 中 board_mask 选择初始化子集。
 * 须在 `nvs_init()` 之后调用 `BoardInit()`。
 */
status_t BoardInit(void);

/** 查询 BoardInit 已成功初始化的板级外设（DEVICE_BOARD_MASK_*）。 */
bool_t BoardPeriphReady(uint32_t mask);

void BoardDeinit(void);

void BoardDeinitI2cBus(void);

/** 已由 `BoardInit` 完成 `st7789_register` 后的句柄；未初始化时返回 NULL。 */
st7789_t *BoardSt7789(void);

/** 已由 `BoardInit` 完成 `gc9a01_register` 后的句柄（desktop_pet）；未初始化时返回 NULL。 */
gc9a01_t *BoardGc9a01(void);

/** 已由 `BoardInit` 完成 `qmi8658a_init_with_config` 后的句柄；未初始化时返回 NULL。 */
qmi8658a_t *BoardQmi8658(void);

/** 已由 `BoardInit` 完成 `ds3231_init_with_config` 后的句柄；未初始化时返回 NULL。 */
ds3231_t *BoardDs3231(void);

/** ballot_guard 红外通道（靠近时对地，空闲为高） */
typedef enum {
    BOARD_IR_CH0 = 0,
    BOARD_IR_CH1 = 1,
    BOARD_IR_CH_COUNT = 2,
} board_ir_channel_e;

typedef struct {
    board_ir_channel_e channel;
} board_ir_event_t;

/** 初始化红外传感器 GPIO（输入上拉 + 下降沿中断）；BoardInit 在 IR 掩码置位时调用。 */
status_t board_ir_init(void);

bool_t board_ir_is_ready(void);

bool_t board_ir_read_level(board_ir_channel_e channel, u32_t *level);

/** 非阻塞取中断事件。 */
bool_t board_ir_take_event(board_ir_event_t *out);

#endif
