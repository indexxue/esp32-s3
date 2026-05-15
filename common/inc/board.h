#ifndef COMMON_BOARD_H
#define COMMON_BOARD_H

#include "type.h"

#include "qmi8658a.h"
#include "spi.h"
#include "st7789.h"

/** 本板硬件引脚与外设接线（换板主要改这里） */
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
 * SD 卡：SDMMC 4 线（与硬件接线一致）。
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

/** IO4 呼吸灯 PWM 频率（与 BoardInit 中配置一致） */
#define BOARD_IO4_PWM_FREQ_HZ (5000U)

/**
 * 板级外设：I2C 总线与设备、SPI+ST7789、QMI8658A 等（换板改 board.h 宏与 board.c 实现）。
 */
status_t BoardInit(void);

void BoardDeinit(void);

void BoardDeinitI2cBus(void);

/** 已由 `BoardInit` 完成 `st7789_register` 后的句柄；未初始化前勿用。 */
st7789_t *BoardSt7789(void);

/** 已由 `BoardInit` 完成 `qmi8658a_init_with_config` 后的句柄。 */
qmi8658a_t *BoardQmi8658(void);

#endif
