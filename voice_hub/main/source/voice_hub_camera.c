#include "voice_hub_camera.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "voice_hub_config.h"
#include "voice_hub_storage.h"

#include "board.h"
#include "dma.h"
#include "i2c.h"
#include "driver/i2c_master.h"
#include "lcd.h"
#include "log.h"
#include "ov2640.h"

static ov2640_t s_ov2640;
static bool_t s_camera_ready;
static st7789_t *s_preview_lcd;
static TaskHandle_t s_preview_task;
static volatile const uint8_t *s_preview_frame;
static volatile uint16_t s_preview_frame_w;
static volatile uint16_t s_preview_frame_h;

#define VOICE_HUB_CAMERA_PREVIEW_TASK_STACK (8192U)
#define VOICE_HUB_CAMERA_PREVIEW_TASK_PRIO (5U)

#if VOICE_HUB_ENABLE_CAMERA

static int voice_hub_ov2640_sccb_write(uint8_t addr7, const uint8_t *data, uint16_t len)
{
    if (I2cWrite((s32_t)BOARD_I2C_OV2640_SCCB_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int voice_hub_ov2640_sccb_read(uint8_t addr7, uint8_t *data, uint16_t len)
{
    if (I2cRead((s32_t)BOARD_I2C_OV2640_SCCB_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int voice_hub_ov2640_sccb_write_read(uint8_t addr7,
                                              const uint8_t *write_data,
                                              uint16_t write_len,
                                              uint8_t *read_data,
                                              uint16_t read_len)
{
    if (I2cWriteRead((s32_t)BOARD_I2C_OV2640_SCCB_PORT,
                     (u16_t)addr7,
                     write_data,
                     (usize_t)write_len,
                     read_data,
                     (usize_t)read_len) != TRUE) {
        return -1;
    }
    return 0;
}

static ov2640_pin_config_t voice_hub_camera_pin_config(void)
{
    ov2640_pin_config_t pins = {
        .pin_xclk  = BOARD_OV2640_PIN_XCLK,
        .pin_pclk  = BOARD_OV2640_PIN_PCLK,
        .pin_vsync = BOARD_OV2640_PIN_VSYNC,
        .pin_href  = BOARD_OV2640_PIN_HREF,
        .pin_d0    = BOARD_OV2640_PIN_D0,
        .pin_d1    = BOARD_OV2640_PIN_D1,
        .pin_d2    = BOARD_OV2640_PIN_D2,
        .pin_d3    = BOARD_OV2640_PIN_D3,
        .pin_d4    = BOARD_OV2640_PIN_D4,
        .pin_d5    = BOARD_OV2640_PIN_D5,
        .pin_d6    = BOARD_OV2640_PIN_D6,
        .pin_d7    = BOARD_OV2640_PIN_D7,
        .pin_pwdn  = BOARD_OV2640_PIN_PWDN,
        .pin_reset = BOARD_OV2640_PIN_RESET,
        .xclk_hz   = BOARD_OV2640_XCLK_HZ,
    };
    return pins;
}

static void voice_hub_i2c_scan_device_cb(void *user_ctx, u16_t address7bit)
{
    (void)user_ctx;
    LOG_INFO("voice_hub I2C scan: ACK 0x%02X%s",
             (unsigned int)address7bit,
             (address7bit == (u16_t)BOARD_I2C_OV2640_SCCB_ADDR) ? " (OV2640)" : "");
}

static void voice_hub_i2c_scan(const char *stage)
{
    u16_t n;

    LOG_INFO("voice_hub I2C scan [%s]: port %d SCL=GPIO%d SDA=GPIO%d, range 0x08..0x77",
             stage,
             (int)BOARD_I2C_OV2640_SCCB_PORT,
             (int)BOARD_I2C_BUS1_PIN_SCL,
             (int)BOARD_I2C_BUS1_PIN_SDA);
    n = I2cScanBus7Bit((s32_t)BOARD_I2C_OV2640_SCCB_PORT, voice_hub_i2c_scan_device_cb, NULL);
    if (n == 0U) {
        LOG_WARN("voice_hub I2C scan [%s]: no device responded", stage);
    } else {
        LOG_INFO("voice_hub I2C scan [%s]: total %u device(s)", stage, (unsigned int)n);
    }

    if (I2cProbe((s32_t)BOARD_I2C_OV2640_SCCB_PORT, (u16_t)BOARD_I2C_OV2640_SCCB_ADDR) == TRUE) {
        LOG_INFO("voice_hub I2C scan [%s]: probe OV2640 @0x%02X OK",
                 stage,
                 (unsigned int)BOARD_I2C_OV2640_SCCB_ADDR);
    } else {
        LOG_WARN("voice_hub I2C scan [%s]: probe OV2640 @0x%02X FAIL (esp err %d)",
                 stage,
                 (unsigned int)BOARD_I2C_OV2640_SCCB_ADDR,
                 (int)I2cGetLastError());
    }
}

static uint32_t voice_hub_camera_strip_bytes(uint16_t row_pixels)
{
    uint32_t row_b = (uint32_t)row_pixels * 2U;
    uint32_t strip_max = (uint32_t)BOARD_ST7789_SPI_MAX_TX;

    strip_max = (strip_max / row_b) * row_b;
    if (strip_max == 0U) {
        strip_max = row_b;
    }
    return strip_max;
}

static void voice_hub_camera_blit_rgb565(st7789_t *lcd, const uint8_t *rgb565, uint16_t cam_w, uint16_t cam_h)
{
    uint16_t lcd_w;
    uint16_t lcd_h;
    uint16_t crop_y;
    uint32_t strip_max;
    void *strip;
    uint16_t row;

    if ((lcd == NULL) || (rgb565 == NULL) || !st7789_is_initialized(lcd)) {
        return;
    }

    lcd_w = st7789_display_width(lcd);
    lcd_h = st7789_display_height(lcd);
    if ((cam_w < lcd_w) || (cam_h < lcd_h)) {
        return;
    }

    crop_y = (uint16_t)((cam_h - lcd_h) / 2U);
    strip_max = voice_hub_camera_strip_bytes(lcd_w);
    strip = DmaMalloc((usize_t)strip_max);
    if (strip == NULL) {
        return;
    }

    if (st7789_set_window(lcd, 0U, 0U, (uint16_t)(lcd_w - 1U), (uint16_t)(lcd_h - 1U)) != ST7789_OK) {
        DmaFree(strip);
        return;
    }

    for (row = 0U; row < lcd_h; row++) {
        uint16_t src_row = (uint16_t)(crop_y + (lcd_h - 1U - row));
        const uint8_t *src = rgb565 + ((uint32_t)src_row * (uint32_t)cam_w * 2U);
        uint32_t remain = (uint32_t)lcd_w * 2U;
        uint32_t offset = 0U;

        while (remain > 0U) {
            uint32_t chunk = (remain > strip_max) ? strip_max : remain;

            memcpy(strip, src + offset, chunk);
            if (st7789_write_pixel_bytes(lcd, (const uint8_t *)strip, chunk) != ST7789_OK) {
                st7789_end_write(lcd);
                DmaFree(strip);
                return;
            }
            offset += chunk;
            remain -= chunk;
        }
    }

    st7789_end_write(lcd);
    DmaFree(strip);
}

static void voice_hub_camera_on_frame(void *user_ctx, const uint8_t *rgb565, uint16_t width, uint16_t height)
{
    BaseType_t wake = pdFALSE;

    (void)user_ctx;
    s_preview_frame   = rgb565;
    s_preview_frame_w = width;
    s_preview_frame_h = height;

    if (s_preview_task != NULL) {
        (void)vTaskNotifyGiveFromISR(s_preview_task, &wake);
        portYIELD_FROM_ISR(wake);
    }
}

static void voice_hub_camera_preview_task(void *arg)
{
    st7789_t *lcd = (st7789_t *)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if ((s_preview_frame == NULL) || (lcd == NULL) || !st7789_is_initialized(lcd)) {
            continue;
        }

        voice_hub_camera_blit_rgb565(lcd,
                                     s_preview_frame,
                                     s_preview_frame_w,
                                     s_preview_frame_h);
    }
}

static status_t voice_hub_camera_preview_task_start(st7789_t *lcd)
{
    if (lcd == NULL) {
        return STATUS_FAIL;
    }

    if (s_preview_task == NULL) {
        if (xTaskCreate(voice_hub_camera_preview_task,
                        "vh_cam",
                        VOICE_HUB_CAMERA_PREVIEW_TASK_STACK,
                        lcd,
                        VOICE_HUB_CAMERA_PREVIEW_TASK_PRIO,
                        &s_preview_task) != pdPASS) {
            LOG_ERROR("voice_hub camera preview task create failed");
            return STATUS_FAIL;
        }
    }

    s_preview_lcd = lcd;
    return STATUS_OK;
}

#endif /* VOICE_HUB_ENABLE_CAMERA */

status_t voice_hub_camera_init(void)
{
#if !VOICE_HUB_ENABLE_CAMERA
    LOG_INFO("voice_hub camera disabled (VOICE_HUB_ENABLE_CAMERA=0)");
    return STATUS_OK;
#else
    ov2640_config_t cfg = {0};
    ov2640_pin_config_t pins;
    i2c_master_bus_handle_t i2c_bus = NULL;

    pins = voice_hub_camera_pin_config();
    if (!ov2640_pins_valid(&pins)) {
        LOG_WARN("OV2640 pins not configured in board.h");
        return STATUS_FAIL;
    }

    if (I2cGetMasterBusHandle((s32_t)BOARD_I2C_OV2640_SCCB_PORT, &i2c_bus) != TRUE) {
        LOG_ERROR("OV2640 I2C bus not ready (port %d)", (int)BOARD_I2C_OV2640_SCCB_PORT);
        return STATUS_FAIL;
    }

    voice_hub_i2c_scan("pre-XCLK");

    cfg.sccb_write         = voice_hub_ov2640_sccb_write;
    cfg.sccb_read          = voice_hub_ov2640_sccb_read;
    cfg.sccb_write_read    = voice_hub_ov2640_sccb_write_read;
    cfg.sccb_addr7         = (uint8_t)BOARD_I2C_OV2640_SCCB_ADDR;
    cfg.pins               = pins;
    cfg.frame_width        = VOICE_HUB_CAMERA_PREVIEW_WIDTH;
    cfg.frame_height       = VOICE_HUB_CAMERA_PREVIEW_HEIGHT;
    cfg.format             = OV2640_FMT_RGB565;
    cfg.i2c_bus_handle     = i2c_bus;
    cfg.i2c_port           = (int8_t)BOARD_I2C_OV2640_SCCB_PORT;
    cfg.i2c_sda            = (int8_t)BOARD_I2C_BUS1_PIN_SDA;
    cfg.i2c_scl            = (int8_t)BOARD_I2C_BUS1_PIN_SCL;
    cfg.sensor_format_name = VOICE_HUB_CAMERA_SENSOR_FORMAT;

    if (ov2640_init_with_config(&s_ov2640, &cfg) != OV2640_OK) {
        LOG_ERROR("ov2640_init_with_config failed");
        return STATUS_FAIL;
    }

    s_camera_ready = TRUE;
    LOG_INFO("OV2640 ready %ux%u", (unsigned)cfg.frame_width, (unsigned)cfg.frame_height);
    return STATUS_OK;
#endif
}

bool_t voice_hub_camera_is_ready(void)
{
    return (s_camera_ready == TRUE) && ov2640_is_initialized(&s_ov2640) ? TRUE : FALSE;
}

status_t voice_hub_camera_preview_start(void)
{
#if !VOICE_HUB_ENABLE_CAMERA
    return STATUS_FAIL;
#else
    st7789_t *lcd = BoardSt7789();

    if (!voice_hub_camera_is_ready()) {
        return STATUS_FAIL;
    }

#if VOICE_HUB_ENABLE_LCD
    if (st7789_is_initialized(lcd)) {
        if (voice_hub_camera_preview_task_start(lcd) != STATUS_OK) {
            return STATUS_FAIL;
        }
        if (ov2640_set_frame_callback(&s_ov2640, voice_hub_camera_on_frame, NULL) != OV2640_OK) {
            LOG_WARN("ov2640_set_frame_callback failed");
        }
    }
#endif

    return ov2640_start_stream(&s_ov2640) == OV2640_OK ? STATUS_OK : STATUS_FAIL;
#endif
}

status_t voice_hub_camera_preview_stop(void)
{
#if !VOICE_HUB_ENABLE_CAMERA
    return STATUS_OK;
#else
    if (!voice_hub_camera_is_ready()) {
        return STATUS_OK;
    }
    s_preview_lcd = NULL;
    return ov2640_stop_stream(&s_ov2640) == OV2640_OK ? STATUS_OK : STATUS_FAIL;
#endif
}

status_t voice_hub_camera_capture_jpeg_to_sd(void)
{
#if !VOICE_HUB_ENABLE_CAMERA || !VOICE_HUB_ENABLE_SDCARD
    return STATUS_FAIL;
#else
    uint8_t  buf[4096];
    uint32_t len = 0U;

    if (!voice_hub_camera_is_ready() || !voice_hub_storage_is_mounted()) {
        return STATUS_FAIL;
    }

    if (ov2640_capture_jpeg(&s_ov2640, buf, sizeof(buf), &len) != OV2640_OK) {
        LOG_WARN("JPEG capture not implemented yet (M4)");
        return STATUS_FAIL;
    }

    return voice_hub_storage_write_jpeg_snapshot(buf, len);
#endif
}
