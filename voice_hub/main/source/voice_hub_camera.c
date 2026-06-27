#include "voice_hub_camera.h"

#include "voice_hub_config.h"
#include "voice_hub_storage.h"

#include "board.h"
#include "i2c.h"
#include "log.h"
#include "ov2640.h"

static ov2640_t s_ov2640;
static bool_t s_camera_ready;

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

#endif /* VOICE_HUB_ENABLE_CAMERA */

status_t voice_hub_camera_init(void)
{
#if !VOICE_HUB_ENABLE_CAMERA
    LOG_INFO("voice_hub camera disabled (VOICE_HUB_ENABLE_CAMERA=0)");
    return STATUS_OK;
#else
    ov2640_config_t cfg = {0};
    ov2640_pin_config_t pins;

    pins = voice_hub_camera_pin_config();
    if (!ov2640_pins_valid(&pins)) {
        LOG_WARN("OV2640 pins not configured in board.h");
        return STATUS_FAIL;
    }

    cfg.sccb_write      = voice_hub_ov2640_sccb_write;
    cfg.sccb_read       = voice_hub_ov2640_sccb_read;
    cfg.sccb_write_read = voice_hub_ov2640_sccb_write_read;
    cfg.sccb_addr7      = (uint8_t)BOARD_I2C_OV2640_SCCB_ADDR;
    cfg.pins            = pins;
    cfg.frame_width     = VOICE_HUB_CAMERA_PREVIEW_WIDTH;
    cfg.frame_height    = VOICE_HUB_CAMERA_PREVIEW_HEIGHT;
    cfg.format          = OV2640_FMT_RGB565;

    if (ov2640_init_with_config(&s_ov2640, &cfg) != OV2640_OK) {
        LOG_ERROR("ov2640_init_with_config failed");
        return STATUS_FAIL;
    }

    s_camera_ready = TRUE;
    LOG_INFO("OV2640 skeleton ready %ux%u", (unsigned)cfg.frame_width, (unsigned)cfg.frame_height);
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
    if (!voice_hub_camera_is_ready()) {
        return STATUS_FAIL;
    }
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
