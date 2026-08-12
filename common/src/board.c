#include "board.h"

#include "battery.h"
#include "device_profile.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gpio.h"
#include "i2c.h"
#if !defined(BOARD_PROFILE_DESKTOP_PET)
#include "lcd.h"
#endif
#include "log.h"
#include "ds3231.h"
#include "sdcard.h"
#include "spi.h"

#include "esp_timer.h"

static qmi8658a_t s_qmi8658;
static ds3231_t s_ds3231;
#if defined(BOARD_PROFILE_DESKTOP_PET)
static gc9a01_t s_gc9a01;
#else
static st7789_t s_st7789;
#endif
static uint32_t s_board_ready_mask;

#define BOARD_IR_QUEUE_LEN (8U)
#define BOARD_IR_DEBOUNCE_US ((int64_t)BOARD_IR_DEBOUNCE_MS * 1000LL)

static const s32_t s_ir_pins[BOARD_IR_CH_COUNT] = {
    (s32_t)BOARD_IR_SENSOR0_PIN,
    (s32_t)BOARD_IR_SENSOR1_PIN,
};

static QueueHandle_t s_ir_queue;
static bool_t s_ir_ready;
static volatile int64_t s_ir_last_evt_us[BOARD_IR_CH_COUNT];

static void IRAM_ATTR board_ir_isr_handler(void *arg)
{
    const board_ir_channel_e ch = (board_ir_channel_e)(uintptr_t)arg;
    board_ir_event_t evt;
    BaseType_t hp = pdFALSE;
    int64_t now;

    if (ch >= BOARD_IR_CH_COUNT) {
        return;
    }

    now = esp_timer_get_time();
    if ((now - s_ir_last_evt_us[ch]) < BOARD_IR_DEBOUNCE_US) {
        return;
    }
    s_ir_last_evt_us[ch] = now;
    evt.channel          = ch;

    if (s_ir_queue != NULL) {
        (void)xQueueSendFromISR(s_ir_queue, &evt, &hp);
        if (hp == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

static status_t board_ir_configure_pin(board_ir_channel_e channel)
{
    GpioPinConfig_t cfg = {0};

    if (channel >= BOARD_IR_CH_COUNT) {
        return STATUS_INVALID_ARG;
    }

    cfg.pin        = s_ir_pins[channel];
    cfg.mode       = GPIO_MODE_INPUT_E;
    cfg.pullUpEn   = GPIO_PULL_ENABLE_E;
    cfg.pullDownEn = GPIO_PULL_DISABLE_E;
    cfg.intrType   = GPIO_INTR_NEGEDGE_E;

    if (GpioConfigurePin(&cfg) != TRUE) {
        LOG_ERROR("IR sensor ch%u GPIO%d configure failed, esp err %d",
                  (unsigned)channel,
                  (int)s_ir_pins[channel],
                  (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    if (GpioRegisterIsr(s_ir_pins[channel], board_ir_isr_handler, (void_t *)(uintptr_t)channel) != TRUE) {
        LOG_ERROR("IR sensor ch%u GPIO%d ISR register failed, esp err %d",
                  (unsigned)channel,
                  (int)s_ir_pins[channel],
                  (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    return STATUS_OK;
}

status_t board_ir_init(void)
{
    board_ir_channel_e ch;

    s_ir_ready = FALSE;

    if (GpioDriverInit() != TRUE) {
        LOG_ERROR("IR sensors: GpioDriverInit failed, esp err %d", (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    if (s_ir_queue == NULL) {
        s_ir_queue = xQueueCreate(BOARD_IR_QUEUE_LEN, sizeof(board_ir_event_t));
        if (s_ir_queue == NULL) {
            LOG_ERROR("IR sensors: event queue create failed");
            return STATUS_FAIL;
        }
    } else {
        (void)xQueueReset(s_ir_queue);
    }

    for (ch = BOARD_IR_CH0; ch < BOARD_IR_CH_COUNT; ch++) {
        s_ir_last_evt_us[ch] = 0;
        if (board_ir_configure_pin(ch) != STATUS_OK) {
            return STATUS_FAIL;
        }
    }

    s_ir_ready = TRUE;
    LOG_INFO("IR sensors ready: ch0 GPIO%d, ch1 GPIO%d (pull-up, negedge, debounce %ums)",
             BOARD_IR_SENSOR0_PIN,
             BOARD_IR_SENSOR1_PIN,
             (unsigned)BOARD_IR_DEBOUNCE_MS);
    return STATUS_OK;
}

bool_t board_ir_is_ready(void)
{
    return s_ir_ready;
}

bool_t board_ir_read_level(board_ir_channel_e channel, u32_t *level)
{
    if ((channel >= BOARD_IR_CH_COUNT) || (level == NULL) || (s_ir_ready != TRUE)) {
        return FALSE;
    }
    return GpioReadPin(s_ir_pins[channel], level);
}

bool_t board_ir_take_event(board_ir_event_t *out)
{
    board_ir_event_t evt;
    bool_t got = FALSE;

    if ((out == NULL) || (s_ir_queue == NULL)) {
        return FALSE;
    }

    while (xQueueReceive(s_ir_queue, &evt, 0) == pdTRUE) {
        *out = evt;
        got  = TRUE;
    }
    return got;
}

bool_t BoardPeriphReady(uint32_t mask)
{
    return (s_board_ready_mask & mask) == mask ? TRUE : FALSE;
}

st7789_t *BoardSt7789(void)
{
#if defined(BOARD_PROFILE_DESKTOP_PET)
    return NULL;
#else
    if (!BoardPeriphReady(DEVICE_BOARD_MASK_LCD)) {
        return NULL;
    }
    return &s_st7789;
#endif
}

gc9a01_t *BoardGc9a01(void)
{
#if defined(BOARD_PROFILE_DESKTOP_PET)
    if (!BoardPeriphReady(DEVICE_BOARD_MASK_LCD)) {
        return NULL;
    }
    return &s_gc9a01;
#else
    return NULL;
#endif
}

qmi8658a_t *BoardQmi8658(void)
{
    if (!BoardPeriphReady(DEVICE_BOARD_MASK_IMU)) {
        return NULL;
    }
    return &s_qmi8658;
}

ds3231_t *BoardDs3231(void)
{
    if (!BoardPeriphReady(DEVICE_BOARD_MASK_RTC)) {
        return NULL;
    }
    return &s_ds3231;
}

static void board_delay_ms(uint32_t ms)
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
    cfg.delay_ms   = board_delay_ms;
    cfg.address    = (uint8_t)BOARD_I2C_QMI8658A_ADDR;
    cfg.accel_range = QMI8658A_ACCEL_RANGE_2G;
    cfg.gyro_range  = QMI8658A_GYRO_RANGE_2048DPS;

    switch (qmi8658a_init_with_config(&s_qmi8658, &cfg)) {
    case QMI8658A_OK:
        LOG_INFO("QMI8658A on I2C port %d init OK", BOARD_I2C_QMI8658A_PORT);
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

static int board_ds3231_i2c_write(uint8_t addr7, const uint8_t *data, uint16_t len)
{
    if (I2cWrite((s32_t)BOARD_I2C_DS3231_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int board_ds3231_i2c_write_read(uint8_t addr7,
                                       const uint8_t *write_data,
                                       uint16_t write_len,
                                       uint8_t *read_data,
                                       uint16_t read_len)
{
    if (I2cWriteRead((s32_t)BOARD_I2C_DS3231_PORT,
                     (u16_t)addr7,
                     write_data,
                     (usize_t)write_len,
                     read_data,
                     (usize_t)read_len) != TRUE) {
        return -1;
    }
    return 0;
}

static status_t board_ds3231_init(void)
{
    ds3231_config_t cfg = {0};
    ds3231_status_flags_t flags = {0};
    ds3231_datetime_t dt = {0};

    cfg.write       = board_ds3231_i2c_write;
    cfg.read        = NULL;
    cfg.write_read  = board_ds3231_i2c_write_read;
    cfg.delay_ms    = board_delay_ms;
    cfg.address     = (uint8_t)BOARD_I2C_DS3231_ADDR;

    if (ds3231_init_with_config(&s_ds3231, &cfg) != DS3231_OK) {
        LOG_ERROR("DS3231 init failed (I2C?) on port %d addr 0x%02X",
                  BOARD_I2C_DS3231_PORT,
                  (unsigned int)BOARD_I2C_DS3231_ADDR);
        return STATUS_FAIL;
    }
    if (ds3231_probe(&s_ds3231) != DS3231_OK) {
        LOG_ERROR("DS3231 not responding on I2C1 SCL=GPIO%d SDA=GPIO%d (check wiring / 0x%02X)",
                  BOARD_I2C_BUS1_PIN_SCL,
                  BOARD_I2C_BUS1_PIN_SDA,
                  (unsigned int)BOARD_I2C_DS3231_ADDR);
        return STATUS_FAIL;
    }

#if BOARD_DS3231_SYNC_TIME_ON_BOOT
    dt.year    = BOARD_DS3231_SYNC_YEAR;
    dt.month   = (uint8_t)BOARD_DS3231_SYNC_MONTH;
    dt.day     = (uint8_t)BOARD_DS3231_SYNC_DAY;
    dt.weekday = (uint8_t)BOARD_DS3231_SYNC_WEEKDAY;
    dt.hour    = (uint8_t)BOARD_DS3231_SYNC_HOUR;
    dt.minute  = (uint8_t)BOARD_DS3231_SYNC_MINUTE;
    dt.second  = (uint8_t)BOARD_DS3231_SYNC_SECOND;
    if (ds3231_write_datetime(&s_ds3231, &dt) != DS3231_OK) {
        LOG_ERROR("DS3231 write_datetime failed");
        return STATUS_FAIL;
    }
    LOG_INFO("DS3231 time synced to %04u-%02u-%02u %02u:%02u:%02u wday=%u",
             (unsigned int)dt.year,
             (unsigned int)dt.month,
             (unsigned int)dt.day,
             (unsigned int)dt.hour,
             (unsigned int)dt.minute,
             (unsigned int)dt.second,
             (unsigned int)dt.weekday);
#endif

    if (ds3231_read_status(&s_ds3231, &flags) == DS3231_OK) {
        if (flags.oscillator_stop) {
            LOG_WARN("DS3231 OSF set (lost power?) — time may be invalid until set");
        }
    }

    if (ds3231_read_datetime(&s_ds3231, &dt) == DS3231_OK) {
        LOG_INFO("DS3231 on I2C1 (port %d) OK: %04u-%02u-%02u %02u:%02u:%02u wday=%u",
                 BOARD_I2C_DS3231_PORT,
                 (unsigned int)dt.year,
                 (unsigned int)dt.month,
                 (unsigned int)dt.day,
                 (unsigned int)dt.hour,
                 (unsigned int)dt.minute,
                 (unsigned int)dt.second,
                 (unsigned int)dt.weekday);
    } else {
        LOG_INFO("DS3231 on I2C1 (port %d) probe OK", BOARD_I2C_DS3231_PORT);
    }

    return STATUS_OK;
}

#if defined(BOARD_PROFILE_DESKTOP_PET)

static void board_gc9a01_spi_tx(const uint8_t *data, uint16_t len)
{
    s32_t cs = (s32_t)BOARD_GC9A01_SPI_DEV_CS_PIN;

    if ((data == NULL) || (len == 0U)) {
        return;
    }
    if (DmaBufferIsBusCapable(data, (usize_t)len) != FALSE) {
        (void)SpiTransmitDma(cs, data, (usize_t)len);
    } else {
        (void)SpiTransmit(cs, data, (usize_t)len);
    }
}

static void board_gc9a01_pin_cs(int high)
{
    (void)GpioWritePin((s32_t)BOARD_GC9A01_PIN_CS, (u32_t)(high ? 1 : 0));
}

static void board_gc9a01_pin_dc(int high)
{
    (void)GpioWritePin((s32_t)BOARD_GC9A01_PIN_DC, (u32_t)(high ? 1 : 0));
}

static void board_gc9a01_pin_rst(int high)
{
    if ((s32_t)BOARD_GC9A01_PIN_RST < 0) {
        return;
    }
    (void)GpioWritePin((s32_t)BOARD_GC9A01_PIN_RST, (u32_t)(high ? 1 : 0));
}

static void board_gc9a01_pin_bl(int high)
{
    (void)GpioWritePin((s32_t)BOARD_GC9A01_PIN_BL, (u32_t)(high ? 1 : 0));
}

static void board_gc9a01_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool_t board_gc9a01_gpio_output_pin(s32_t pin)
{
    GpioPinConfig_t cfg = {0};

    cfg.pin        = pin;
    cfg.mode       = GPIO_MODE_OUTPUT_E;
    cfg.pullUpEn   = GPIO_PULL_DISABLE_E;
    cfg.pullDownEn = GPIO_PULL_DISABLE_E;
    cfg.intrType   = GPIO_INTR_DISABLE_E;

    return GpioConfigurePin(&cfg);
}

static void board_gc9a01_smoke_test(void)
{
    uint16_t w = gc9a01_display_width(&s_gc9a01);
    uint16_t h = gc9a01_display_height(&s_gc9a01);
    const char *title = device_profile_lcd_smoke_title();

    gc9a01_fill(&s_gc9a01, 0U, 0U, w, h, 0x01CFU); /* dark blue RGB565 */
    LOG_INFO("GC9A01 smoke: %s, %ux%u", title, (unsigned int)w, (unsigned int)h);
}

static status_t board_gc9a01_init(void)
{
    SpiDriverConfig_t busCfg = {0};
    SpiDeviceConfig_t devCfg = {0};
    gc9a01_config_t tftCfg = {0};

    if (GpioDriverInit() != TRUE) {
        LOG_ERROR("GC9A01: GpioDriverInit failed, esp err %d", (int)GpioGetLastError());
        return STATUS_FAIL;
    }

    if (board_gc9a01_gpio_output_pin((s32_t)BOARD_GC9A01_PIN_CS) != TRUE) {
        LOG_ERROR("GC9A01: CS GPIO%d failed", BOARD_GC9A01_PIN_CS);
        return STATUS_FAIL;
    }
    if (board_gc9a01_gpio_output_pin((s32_t)BOARD_GC9A01_PIN_DC) != TRUE) {
        LOG_ERROR("GC9A01: DC GPIO%d failed", BOARD_GC9A01_PIN_DC);
        return STATUS_FAIL;
    }
    if ((s32_t)BOARD_GC9A01_PIN_RST >= 0) {
        if (board_gc9a01_gpio_output_pin((s32_t)BOARD_GC9A01_PIN_RST) != TRUE) {
            LOG_ERROR("GC9A01: RST GPIO%d failed", BOARD_GC9A01_PIN_RST);
            return STATUS_FAIL;
        }
    }
    if (board_gc9a01_gpio_output_pin((s32_t)BOARD_GC9A01_PIN_BL) != TRUE) {
        LOG_ERROR("GC9A01: BL GPIO%d failed", BOARD_GC9A01_PIN_BL);
        return STATUS_FAIL;
    }

    busCfg.host            = BOARD_GC9A01_SPI_HOST;
    busCfg.sclkPin         = (s32_t)BOARD_GC9A01_PIN_SCK;
    busCfg.mosiPin         = (s32_t)BOARD_GC9A01_PIN_MOSI;
    busCfg.misoPin         = (s32_t)BOARD_GC9A01_PIN_MISO;
    busCfg.quadWpPin       = -1;
    busCfg.quadHdPin       = -1;
    busCfg.maxTransferSize = (s32_t)BOARD_GC9A01_SPI_MAX_TX;
    busCfg.dmaChannel      = DMA_SPI_BUS_AUTO_E;
    busCfg.intrFlags       = 0;
    busCfg.maxDeviceCount  = 2U;

    if (SpiDriverInit(&busCfg) != TRUE) {
        LOG_ERROR("GC9A01: SpiDriverInit failed, esp err %d", (int)SpiGetLastError());
        return STATUS_FAIL;
    }

    devCfg.host           = BOARD_GC9A01_SPI_HOST;
    devCfg.chipSelectPin  = (s32_t)BOARD_GC9A01_SPI_DEV_CS_PIN;
    devCfg.clockSpeedHz   = BOARD_GC9A01_SPI_CLOCK_HZ;
    devCfg.mode           = SPI_CLOCK_MODE_0_E;
    devCfg.flags          = 0U;
    devCfg.queueSize      = 7U;
    devCfg.csEnaPretrans  = 0U;
    devCfg.csEnaPosttrans = 0U;

    if (SpiRegisterDevice(&devCfg) != TRUE) {
        LOG_ERROR("GC9A01: SpiRegisterDevice failed, esp err %d", (int)SpiGetLastError());
        return STATUS_FAIL;
    }

    tftCfg.spi_tx   = board_gc9a01_spi_tx;
    tftCfg.set_cs   = board_gc9a01_pin_cs;
    tftCfg.set_dc   = board_gc9a01_pin_dc;
    tftCfg.set_rst  = ((s32_t)BOARD_GC9A01_PIN_RST >= 0) ? board_gc9a01_pin_rst : NULL;
    tftCfg.set_bl   = board_gc9a01_pin_bl;
    tftCfg.delay_ms = board_gc9a01_delay_ms;
    tftCfg.rotation = (uint8_t)GC9A01_ROT_0;

    switch (gc9a01_register(&s_gc9a01, &tftCfg)) {
    case GC9A01_OK:
        LOG_INFO("GC9A01 SPI SCK=%d MOSI=%d MISO=%d CS=%d DC=%d RST=%d BL=%d init OK",
                 BOARD_GC9A01_PIN_SCK,
                 BOARD_GC9A01_PIN_MOSI,
                 BOARD_GC9A01_PIN_MISO,
                 BOARD_GC9A01_PIN_CS,
                 BOARD_GC9A01_PIN_DC,
                 BOARD_GC9A01_PIN_RST,
                 BOARD_GC9A01_PIN_BL);
        board_gc9a01_smoke_test();
        return STATUS_OK;
    case GC9A01_ERROR_PARAM:
        LOG_ERROR("GC9A01 register failed: bad param");
        return STATUS_FAIL;
    default:
        LOG_ERROR("GC9A01 register failed");
        return STATUS_FAIL;
    }
}

#else /* !BOARD_PROFILE_DESKTOP_PET — ST7789 path */

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
    if ((s32_t)BOARD_ST7789_PIN_RST < 0) {
        return;
    }
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
    const char *title    = device_profile_lcd_smoke_title();
    const char *subtitle = device_profile_lcd_smoke_subtitle();

    lcd_fill(&s_st7789, 0U, 0U, w, h, LCD_COLOR_DARKBLUE);
    lcd_draw_rectangle(&s_st7789, 0U, 0U, (uint16_t)(w - 1U), (uint16_t)(h - 1U), LCD_COLOR_YELLOW);
    lcd_show_string(&s_st7789, 8U, 16U, (const uint8_t *)title, LCD_COLOR_WHITE, LCD_COLOR_DARKBLUE, 16U, 0U);
    lcd_show_string(&s_st7789, 8U, 40U, (const uint8_t *)subtitle, LCD_COLOR_CYAN, LCD_COLOR_DARKBLUE, 16U, 0U);
    LOG_INFO("LCD smoke: %s / %s, %ux%u", title, subtitle, (unsigned int)w, (unsigned int)h);
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
    if ((s32_t)BOARD_ST7789_PIN_RST >= 0) {
        if (board_st7789_gpio_output_pin((s32_t)BOARD_ST7789_PIN_RST) != TRUE) {
            LOG_ERROR("ST7789: GpioConfigurePin RST GPIO%d failed", BOARD_ST7789_PIN_RST);
            return STATUS_FAIL;
        }
    }
    if (board_st7789_gpio_output_pin((s32_t)BOARD_ST7789_PIN_BL) != TRUE) {
        LOG_ERROR("ST7789: GpioConfigurePin BL GPIO%d failed", BOARD_ST7789_PIN_BL);
        return STATUS_FAIL;
    }

    busCfg.host            = BOARD_ST7789_SPI_HOST;
    busCfg.sclkPin         = (s32_t)BOARD_ST7789_PIN_SCK;
    busCfg.mosiPin         = (s32_t)BOARD_ST7789_PIN_MOSI;
#if defined(BOARD_ST7789_PIN_MISO)
    busCfg.misoPin         = (s32_t)BOARD_ST7789_PIN_MISO;
#else
    busCfg.misoPin         = -1;
#endif
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

    devCfg.host           = BOARD_ST7789_SPI_HOST;
    devCfg.chipSelectPin  = (s32_t)BOARD_ST7789_SPI_DEV_CS_PIN;
    devCfg.clockSpeedHz   = BOARD_ST7789_SPI_CLOCK_HZ;
    devCfg.mode           = SPI_CLOCK_MODE_0_E;
    devCfg.flags          = 0U;
    devCfg.queueSize      = 7U;
    devCfg.csEnaPretrans  = 0U;
    devCfg.csEnaPosttrans = 0U;

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

#endif /* BOARD_PROFILE_DESKTOP_PET */

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

static status_t board_init_i2c_bus1(void)
{
    I2cDriverConfig_t bus1Cfg = {0};

    bus1Cfg.port = BOARD_I2C_BUS1_HW_PORT;
    bus1Cfg.sdaPin = BOARD_I2C_BUS1_PIN_SDA;
    bus1Cfg.sclPin = BOARD_I2C_BUS1_PIN_SCL;
    bus1Cfg.defaultClockSpeedHz = BOARD_I2C_DEFAULT_CLOCK_HZ;
    bus1Cfg.defaultTransactionTimeoutMs = BOARD_I2C_DEFAULT_TIMEOUT_MS;
    bus1Cfg.maxDeviceCount = BOARD_I2C_MAX_DEVICES;
    bus1Cfg.glitchIgnoreCount = BOARD_I2C_GLITCH_IGNORE;
    bus1Cfg.enableSdaPullup = TRUE;
    bus1Cfg.enableSclPullup = TRUE;

    if (I2cDriverInit(&bus1Cfg) != TRUE) {
        LOG_ERROR("I2cDriverInit I2C1 (port %d) failed, esp err %d",
                  BOARD_I2C_BUS1_HW_PORT,
                  (int)I2cGetLastError());
        return STATUS_FAIL;
    }

#if BOARD_I2C_BUS1_SCAN_ON_BOOT
    board_i2c_bus1_scan_log();
#endif

    return STATUS_OK;
}

static status_t board_register_qmi8658a_i2c(void)
{
    I2cDeviceConfig_t qmiCfg = {0};

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

static status_t board_init_i2c_bus2(void)
{
    /* desktop_pet 等单总线板：BUS2 引脚为 -1，不创建第二路 master。 */
    if ((BOARD_I2C_BUS2_PIN_SDA < 0) || (BOARD_I2C_BUS2_PIN_SCL < 0)) {
        if (BOARD_I2C_QMI8658A_PORT == BOARD_I2C_BUS2_HW_PORT) {
            LOG_ERROR("I2C2 pins invalid but QMI8658A assigned to port %d",
                      BOARD_I2C_BUS2_HW_PORT);
            return STATUS_FAIL;
        }
        return STATUS_OK;
    }

    {
        I2cDriverConfig_t bus2Cfg = {0};

        bus2Cfg.port = BOARD_I2C_BUS2_HW_PORT;
        bus2Cfg.sdaPin = BOARD_I2C_BUS2_PIN_SDA;
        bus2Cfg.sclPin = BOARD_I2C_BUS2_PIN_SCL;
        bus2Cfg.defaultClockSpeedHz = BOARD_I2C_DEFAULT_CLOCK_HZ;
        bus2Cfg.defaultTransactionTimeoutMs = BOARD_I2C_DEFAULT_TIMEOUT_MS;
        bus2Cfg.maxDeviceCount = BOARD_I2C_MAX_DEVICES;
        bus2Cfg.glitchIgnoreCount = BOARD_I2C_GLITCH_IGNORE;
        bus2Cfg.enableSdaPullup = TRUE;
        bus2Cfg.enableSclPullup = TRUE;

        if (I2cDriverInit(&bus2Cfg) != TRUE) {
            LOG_ERROR("I2cDriverInit I2C2 (port %d) failed, esp err %d",
                      BOARD_I2C_BUS2_HW_PORT,
                      (int)I2cGetLastError());
            return STATUS_FAIL;
        }
    }

    return STATUS_OK;
}

static status_t board_register_ds3231_i2c(void)
{
    I2cDeviceConfig_t rtcCfg = {0};

    rtcCfg.port = BOARD_I2C_DS3231_PORT;
    rtcCfg.deviceAddress7bit = BOARD_I2C_DS3231_ADDR;
    rtcCfg.clockSpeedHz = 0U;
    rtcCfg.transactionTimeoutMs = 0U;

    if (I2cRegisterDevice(&rtcCfg) != TRUE) {
        LOG_ERROR("I2cRegisterDevice DS3231 0x%02X on port %d failed, esp err %d",
                  (unsigned int)BOARD_I2C_DS3231_ADDR,
                  BOARD_I2C_DS3231_PORT,
                  (int)I2cGetLastError());
        return STATUS_FAIL;
    }

    return STATUS_OK;
}

static status_t board_init_i2c(void)
{
    if (board_init_i2c_bus1() != STATUS_OK) {
        return STATUS_FAIL;
    }

    /*
     * IMU 可能挂 I2C1（desktop_pet）或 I2C2（量产板）。
     * 先按需初始化 bus2（有有效引脚时），再在 BOARD_I2C_QMI8658A_PORT 上注册。
     */
    if (device_profile_board_wants(DEVICE_BOARD_MASK_IMU)) {
        if (board_init_i2c_bus2() != STATUS_OK) {
            return STATUS_FAIL;
        }
        if (board_register_qmi8658a_i2c() != STATUS_OK) {
            return STATUS_FAIL;
        }
    }

    if (device_profile_board_wants(DEVICE_BOARD_MASK_RTC)) {
        if (board_register_ds3231_i2c() != STATUS_OK) {
            return STATUS_FAIL;
        }
    }

    return STATUS_OK;
}

status_t BoardInit(void)
{
    const device_product_profile_t *product = device_profile_product();
    const uint32_t board_mask = device_profile_board_mask();
    const uint32_t hw_id = device_profile_hardware_id();
    const char *hw_name = device_profile_hardware_name(hw_id);

    s_board_ready_mask = 0U;

    LOG_INFO("BoardInit: product=%s (0x%08lX) hardware=%s (0x%08lX) board_mask=0x%02lX",
             product->name,
             (unsigned long)product->product_id,
             (hw_name != NULL) ? hw_name : "?",
             (unsigned long)hw_id,
             (unsigned long)board_mask);

    if (device_profile_board_wants(DEVICE_BOARD_MASK_I2C)) {
        if (board_init_i2c() != STATUS_OK) {
            return STATUS_FAIL;
        }
        s_board_ready_mask |= DEVICE_BOARD_MASK_I2C;
    }

    if (device_profile_board_wants(DEVICE_BOARD_MASK_RTC)) {
        if (!device_profile_board_wants(DEVICE_BOARD_MASK_I2C)) {
            LOG_ERROR("DS3231 requires DEVICE_BOARD_MASK_I2C in board_mask");
            return STATUS_FAIL;
        }
        if (board_ds3231_init() != STATUS_OK) {
            return STATUS_FAIL;
        }
        s_board_ready_mask |= DEVICE_BOARD_MASK_RTC;
    }

    if (device_profile_board_wants(DEVICE_BOARD_MASK_SDCARD)) {
#if defined(BOARD_PROFILE_DESKTOP_PET) && !DESKTOP_PET_ENABLE_SDCARD
        LOG_INFO("SD skipped (DESKTOP_PET_ENABLE_SDCARD=0)");
#else
        if (sdcard_mount(BOARD_SDCARD_MOUNT_POINT) != STATUS_OK) {
            LOG_WARN("SD card FAT mount skipped or failed (check card / wiring), path %s", BOARD_SDCARD_MOUNT_POINT);
        } else {
            s_board_ready_mask |= DEVICE_BOARD_MASK_SDCARD;
        }
#endif
    }

    if (device_profile_board_wants(DEVICE_BOARD_MASK_LCD)) {
#if defined(BOARD_PROFILE_CAMERA) && !CAMERA_ENABLE_LCD
        LOG_INFO("LCD skipped (CAMERA_ENABLE_LCD=0)");
#elif defined(BOARD_PROFILE_VOICE_HUB) && !VOICE_HUB_ENABLE_LCD
        LOG_INFO("LCD skipped (VOICE_HUB_ENABLE_LCD=0)");
#elif defined(BOARD_PROFILE_DESKTOP_PET) && !DESKTOP_PET_ENABLE_LCD
        LOG_INFO("LCD skipped (DESKTOP_PET_ENABLE_LCD=0; GC9A01)");
#elif defined(BOARD_PROFILE_DESKTOP_PET)
        if (board_gc9a01_init() != STATUS_OK) {
            return STATUS_FAIL;
        }
        s_board_ready_mask |= DEVICE_BOARD_MASK_LCD;
#else
        if (board_st7789_init() != STATUS_OK) {
            return STATUS_FAIL;
        }
        s_board_ready_mask |= DEVICE_BOARD_MASK_LCD;
#endif
    }

    if (device_profile_board_wants(DEVICE_BOARD_MASK_BATTERY)) {
        battery_init();
        s_board_ready_mask |= DEVICE_BOARD_MASK_BATTERY;
    }

    if (device_profile_board_wants(DEVICE_BOARD_MASK_IMU)) {
#if defined(BOARD_PROFILE_DESKTOP_PET) && !DESKTOP_PET_ENABLE_IMU
        LOG_INFO("IMU skipped (DESKTOP_PET_ENABLE_IMU=0)");
#else
        if (board_qmi8658_init() != STATUS_OK) {
            return STATUS_FAIL;
        }
        s_board_ready_mask |= DEVICE_BOARD_MASK_IMU;
#endif
    }

    if (device_profile_board_wants(DEVICE_BOARD_MASK_IR)) {
        if (board_ir_init() != STATUS_OK) {
            LOG_WARN("IR sensors init failed (GPIO%d/GPIO%d)", BOARD_IR_SENSOR0_PIN, BOARD_IR_SENSOR1_PIN);
        } else {
            s_board_ready_mask |= DEVICE_BOARD_MASK_IR;
        }
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
