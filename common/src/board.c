#include "board.h"

#include "battery.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpio.h"
#include "i2c.h"
#include "lcd.h"
#include "log.h"
#include "sdcard.h"
#include "spi.h"

static qmi8658a_t s_qmi8658;
static st7789_t s_st7789;

st7789_t *BoardSt7789(void)
{
    return &s_st7789;
}

qmi8658a_t *BoardQmi8658(void)
{
    return &s_qmi8658;
}

static void board_qmi_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static int board_qmi_i2c_write(uint8_t addr7, const uint8_t *data, uint16_t len)
{
    if (I2cWrite((s32_t)BOARD_I2C_QMI8658A_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int board_qmi_i2c_write_read(uint8_t addr7,
                                    const uint8_t *write_data,
                                    uint16_t write_len,
                                    uint8_t *read_data,
                                    uint16_t read_len)
{
    if (I2cWriteRead((s32_t)BOARD_I2C_QMI8658A_PORT,
                     (u16_t)addr7,
                     write_data,
                     (usize_t)write_len,
                     read_data,
                     (usize_t)read_len) != TRUE) {
        return -1;
    }
    return 0;
}

static status_t board_qmi8658_init(void)
{
    qmi8658a_config_t cfg = {0};

    cfg.write      = board_qmi_i2c_write;
    cfg.read       = NULL;
    cfg.write_read = board_qmi_i2c_write_read;
    cfg.delay_ms   = board_qmi_delay_ms;
    cfg.address    = (uint8_t)BOARD_I2C_QMI8658A_ADDR;
    cfg.accel_range = QMI8658A_ACCEL_RANGE_2G;
    cfg.gyro_range  = QMI8658A_GYRO_RANGE_2048DPS;

    switch (qmi8658a_init_with_config(&s_qmi8658, &cfg)) {
    case QMI8658A_OK:
        LOG_INFO("QMI8658A on I2C2 (port %d) init OK", BOARD_I2C_QMI8658A_PORT);
        return STATUS_OK;
    case QMI8658A_ERROR_ID:
        LOG_ERROR("QMI8658A WHO_AM_I mismatch (check wiring / address 0x%02X)",
                  (unsigned int)BOARD_I2C_QMI8658A_ADDR);
        return STATUS_FAIL;
    default:
        LOG_ERROR("QMI8658A init failed (I2C?)");
        return STATUS_FAIL;
    }
}

static void board_st7789_spi_tx(const uint8_t *data, uint16_t len)
{
    s32_t cs = (s32_t)BOARD_ST7789_SPI_DEV_CS_PIN;

    if ((data == NULL) || (len == 0U)) {
        return;
    }
    if (DmaBufferIsBusCapable(data, (usize_t)len) != FALSE) {
        (void)SpiTransmitDma(cs, data, (usize_t)len);
    } else {
        (void)SpiTransmit(cs, data, (usize_t)len);
    }
}

static void board_st7789_pin_cs(int high)
{
    (void)GpioWritePin((s32_t)BOARD_ST7789_PIN_CS, (u32_t)(high ? 1 : 0));
}

static void board_st7789_pin_dc(int high)
{
    (void)GpioWritePin((s32_t)BOARD_ST7789_PIN_DC, (u32_t)(high ? 1 : 0));
}

static void board_st7789_pin_rst(int high)
{
    (void)GpioWritePin((s32_t)BOARD_ST7789_PIN_RST, (u32_t)(high ? 1 : 0));
}

static void board_st7789_pin_bl(int high)
{
    (void)GpioWritePin((s32_t)BOARD_ST7789_PIN_BL, (u32_t)(high ? 1 : 0));
}

static void board_st7789_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool_t board_st7789_gpio_output_pin(s32_t pin)
{
    GpioPinConfig_t cfg = {0};

    cfg.pin        = pin;
    cfg.mode       = GPIO_MODE_OUTPUT_E;
    cfg.pullUpEn   = GPIO_PULL_DISABLE_E;
    cfg.pullDownEn = GPIO_PULL_DISABLE_E;
    cfg.intrType   = GPIO_INTR_DISABLE_E;

    return GpioConfigurePin(&cfg);
}

static void board_lcd_smoke_test(void)
{
    uint16_t w = st7789_display_width(&s_st7789);
    uint16_t h = st7789_display_height(&s_st7789);

    lcd_fill(&s_st7789, 0U, 0U, w, h, LCD_COLOR_DARKBLUE);
    lcd_draw_rectangle(&s_st7789, 0U, 0U, (uint16_t)(w - 1U), (uint16_t)(h - 1U), LCD_COLOR_YELLOW);
    lcd_show_string(&s_st7789, 8U, 16U, (const uint8_t *)"ST7789 + lcd OK", LCD_COLOR_WHITE, LCD_COLOR_DARKBLUE, 16U, 0U);
    lcd_show_string(&s_st7789, 8U, 40U, (const uint8_t *)"ESP32-S3 smoke test", LCD_COLOR_CYAN, LCD_COLOR_DARKBLUE, 16U, 0U);
    LOG_INFO("LCD smoke: filled %ux%u, border + 2 lines (16px font)", (unsigned int)w, (unsigned int)h);
}

static status_t board_st7789_init(void)
{
    SpiDriverConfig_t busCfg = {0};
    SpiDeviceConfig_t devCfg = {0};
    st7789_config_t tftCfg  = {0};

    if (GpioDriverInit() != TRUE) {
        LOG_ERROR("ST7789: GpioDriverInit failed, esp err %d", (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    if (board_st7789_gpio_output_pin((s32_t)BOARD_ST7789_PIN_CS) != TRUE) {
        LOG_ERROR("ST7789: GpioConfigurePin CS GPIO%d failed", BOARD_ST7789_PIN_CS);
        return STATUS_FAIL;
    }
    if (board_st7789_gpio_output_pin((s32_t)BOARD_ST7789_PIN_DC) != TRUE) {
        LOG_ERROR("ST7789: GpioConfigurePin DC GPIO%d failed", BOARD_ST7789_PIN_DC);
        return STATUS_FAIL;
    }
    if (board_st7789_gpio_output_pin((s32_t)BOARD_ST7789_PIN_RST) != TRUE) {
        LOG_ERROR("ST7789: GpioConfigurePin RST GPIO%d failed", BOARD_ST7789_PIN_RST);
        return STATUS_FAIL;
    }
    if (board_st7789_gpio_output_pin((s32_t)BOARD_ST7789_PIN_BL) != TRUE) {
        LOG_ERROR("ST7789: GpioConfigurePin BL GPIO%d failed", BOARD_ST7789_PIN_BL);
        return STATUS_FAIL;
    }

    busCfg.host            = BOARD_ST7789_SPI_HOST;
    busCfg.sclkPin         = (s32_t)BOARD_ST7789_PIN_SCK;
    busCfg.mosiPin         = (s32_t)BOARD_ST7789_PIN_MOSI;
    busCfg.misoPin         = -1;
    busCfg.quadWpPin       = -1;
    busCfg.quadHdPin       = -1;
    busCfg.maxTransferSize = (s32_t)BOARD_ST7789_SPI_MAX_TX;
    busCfg.dmaChannel      = DMA_SPI_BUS_AUTO_E;
    busCfg.intrFlags       = 0;
    busCfg.maxDeviceCount  = 2U;

    if (SpiDriverInit(&busCfg) != TRUE) {
        LOG_ERROR("ST7789: SpiDriverInit failed, esp err %d", (int)SpiGetLastError());
        return STATUS_FAIL;
    }

    devCfg.chipSelectPin  = (s32_t)BOARD_ST7789_SPI_DEV_CS_PIN;
    devCfg.clockSpeedHz   = BOARD_ST7789_SPI_CLOCK_HZ;
    devCfg.mode           = SPI_CLOCK_MODE_0_E;
    devCfg.flags          = 0U;
    devCfg.queueSize      = 7U;

    if (SpiRegisterDevice(&devCfg) != TRUE) {
        LOG_ERROR("ST7789: SpiRegisterDevice failed, esp err %d", (int)SpiGetLastError());
        return STATUS_FAIL;
    }

    tftCfg.spi_tx   = board_st7789_spi_tx;
    tftCfg.set_cs   = board_st7789_pin_cs;
    tftCfg.set_dc   = board_st7789_pin_dc;
    tftCfg.set_rst  = board_st7789_pin_rst;
    tftCfg.set_bl   = board_st7789_pin_bl;
    tftCfg.delay_ms = board_st7789_delay_ms;
    tftCfg.rotation = (uint8_t)ST7789_ROT_LANDSCAPE;

    switch (st7789_register(&s_st7789, &tftCfg)) {
    case ST7789_OK:
        LOG_INFO("ST7789 SPI2 SCK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d rot=LANDSCAPE init OK",
                 BOARD_ST7789_PIN_SCK,
                 BOARD_ST7789_PIN_MOSI,
                 BOARD_ST7789_PIN_CS,
                 BOARD_ST7789_PIN_DC,
                 BOARD_ST7789_PIN_RST,
                 BOARD_ST7789_PIN_BL);
        board_lcd_smoke_test();
        return STATUS_OK;
    case ST7789_ERROR_PARAM:
        LOG_ERROR("ST7789 register failed: bad param");
        return STATUS_FAIL;
    default:
        LOG_ERROR("ST7789 register failed");
        return STATUS_FAIL;
    }
}

#if BOARD_I2C_BUS1_SCAN_ON_BOOT
static void board_i2c1_scan_device_cb(void *user_ctx, u16_t address7bit)
{
    (void)user_ctx;
    LOG_INFO("I2C1 scan: ACK at 7-bit addr 0x%02X", (unsigned int)address7bit);
}

static void board_i2c_bus1_scan_log(void)
{
    u16_t n;

    LOG_INFO("I2C1 scan: port %d SDA=GPIO%d SCL=GPIO%d, range 0x08..0x77",
             (int)BOARD_I2C_BUS1_HW_PORT,
             (int)BOARD_I2C_BUS1_PIN_SDA,
             (int)BOARD_I2C_BUS1_PIN_SCL);
    n = I2cScanBus7Bit((s32_t)BOARD_I2C_BUS1_HW_PORT, board_i2c1_scan_device_cb, NULL);
    if (n == 0U) {
        LOG_WARN("I2C1 scan: no device responded (check wiring / pull-ups / bus power)");
    } else {
        LOG_INFO("I2C1 scan: total %u device(s)", (unsigned int)n);
    }
}
#endif

static status_t board_init_i2c(void)
{
    I2cDriverConfig_t bus1Cfg = {0};
    I2cDriverConfig_t bus2Cfg = {0};
    I2cDeviceConfig_t qmiCfg = {0};

    bus1Cfg.port = BOARD_I2C_BUS1_HW_PORT;
    bus1Cfg.sdaPin = BOARD_I2C_BUS1_PIN_SDA;
    bus1Cfg.sclPin = BOARD_I2C_BUS1_PIN_SCL;
    bus1Cfg.defaultClockSpeedHz = BOARD_I2C_DEFAULT_CLOCK_HZ;
    bus1Cfg.defaultTransactionTimeoutMs = BOARD_I2C_DEFAULT_TIMEOUT_MS;
    bus1Cfg.maxDeviceCount = BOARD_I2C_MAX_DEVICES;
    bus1Cfg.glitchIgnoreCount = BOARD_I2C_GLITCH_IGNORE;
    bus1Cfg.enableSdaPullup = TRUE;
    bus1Cfg.enableSclPullup = TRUE;

    bus2Cfg.port = BOARD_I2C_BUS2_HW_PORT;
    bus2Cfg.sdaPin = BOARD_I2C_BUS2_PIN_SDA;
    bus2Cfg.sclPin = BOARD_I2C_BUS2_PIN_SCL;
    bus2Cfg.defaultClockSpeedHz = BOARD_I2C_DEFAULT_CLOCK_HZ;
    bus2Cfg.defaultTransactionTimeoutMs = BOARD_I2C_DEFAULT_TIMEOUT_MS;
    bus2Cfg.maxDeviceCount = BOARD_I2C_MAX_DEVICES;
    bus2Cfg.glitchIgnoreCount = BOARD_I2C_GLITCH_IGNORE;
    bus2Cfg.enableSdaPullup = TRUE;
    bus2Cfg.enableSclPullup = TRUE;

    if (I2cDriverInit(&bus1Cfg) != TRUE) {
        LOG_ERROR("I2cDriverInit I2C1 (port %d) failed, esp err %d",
                  BOARD_I2C_BUS1_HW_PORT,
                  (int)I2cGetLastError());
        return STATUS_FAIL;
    }
    if (I2cDriverInit(&bus2Cfg) != TRUE) {
        LOG_ERROR("I2cDriverInit I2C2 (port %d) failed, esp err %d",
                  BOARD_I2C_BUS2_HW_PORT,
                  (int)I2cGetLastError());
        return STATUS_FAIL;
    }

#if BOARD_I2C_BUS1_SCAN_ON_BOOT
    board_i2c_bus1_scan_log();
#endif

    qmiCfg.port = BOARD_I2C_QMI8658A_PORT;
    qmiCfg.deviceAddress7bit = BOARD_I2C_QMI8658A_ADDR;
    qmiCfg.clockSpeedHz = 0U;
    qmiCfg.transactionTimeoutMs = 0U;

    if (I2cRegisterDevice(&qmiCfg) != TRUE) {
        LOG_ERROR("I2cRegisterDevice QMI8658A 0x%02X on port %d failed, esp err %d",
                  (unsigned int)BOARD_I2C_QMI8658A_ADDR,
                  BOARD_I2C_QMI8658A_PORT,
                  (int)I2cGetLastError());
        return STATUS_FAIL;
    }

    return STATUS_OK;
}

status_t BoardInit(void)
{
    if (board_init_i2c() != STATUS_OK) {
        return STATUS_FAIL;
    }

    if (board_st7789_init() != STATUS_OK) {
        return STATUS_FAIL;
    }

    battery_init();

    if (board_qmi8658_init() != STATUS_OK) {
        return STATUS_FAIL;
    }

    if (sdcard_mount(BOARD_SDCARD_MOUNT_POINT) != STATUS_OK) {
        LOG_WARN("SD card FAT mount skipped or failed (check card / wiring), path %s", BOARD_SDCARD_MOUNT_POINT);
    }

    return STATUS_OK;
}

void BoardDeinit(void)
{
    if (sdcard_get_card() != NULL) {
        (void)sdcard_unmount(BOARD_SDCARD_MOUNT_POINT);
    }
}

void BoardDeinitI2cBus(void)
{
}
