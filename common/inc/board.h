#ifndef COMMON_BOARD_H
#define COMMON_BOARD_H

#include "type.h"

#include "device_profile.h"
#include "ds3231.h"
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

/** IO4 呼吸灯 PWM 频率（与 BoardInit 中配置一致） */
#define BOARD_IO4_PWM_FREQ_HZ (5000U)

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
