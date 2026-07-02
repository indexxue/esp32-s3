#include "voice_hub_camera.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_jpeg_common.h"
#include "esp_jpeg_enc.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "voice_hub_storage.h"
#include "voice_hub_ui.h"

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
#define VOICE_HUB_CAMERA_JPEG_OUT_CAP (49152U)

#if VOICE_HUB_ENABLE_CAMERA

static uint8_t *s_snapshot_buf;
static uint8_t *s_snapshot_copy;
static uint32_t s_snapshot_bytes;
static uint16_t s_snapshot_w;
static uint16_t s_snapshot_h;
static SemaphoreHandle_t s_snapshot_mtx;
static SemaphoreHandle_t s_jpeg_mutex;
static jpeg_enc_handle_t s_jpeg_enc;
static uint8_t *s_blit_strip;
static uint32_t s_blit_strip_cap;
static volatile uint8_t s_flip_vertical   = (uint8_t)VOICE_HUB_CAMERA_FLIP_VERTICAL;
static volatile uint8_t s_flip_horizontal = (uint8_t)VOICE_HUB_CAMERA_FLIP_HORIZONTAL;
static volatile u16_t   s_rotate_deg        = 0U;

static void *voice_hub_camera_buf_alloc(uint32_t size)
{
    void *p;

    p = heap_caps_aligned_alloc(16, (size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_aligned_alloc(16, (size_t)size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void voice_hub_camera_buf_free(void *ptr)
{
    if (ptr != NULL) {
        heap_caps_free(ptr);
    }
}

void voice_hub_camera_view_get(voice_hub_camera_view_t *out)
{
    if (out == NULL) {
        return;
    }
    out->flip_vertical   = (s_flip_vertical != 0U) ? TRUE : FALSE;
    out->flip_horizontal = (s_flip_horizontal != 0U) ? TRUE : FALSE;
    out->rotate_deg      = s_rotate_deg;
}

status_t voice_hub_camera_view_set(const voice_hub_camera_view_t *view)
{
    if (view == NULL) {
        return STATUS_INVALID_ARG;
    }

    switch (view->rotate_deg) {
    case 0U:
    case 90U:
    case 180U:
    case 270U:
        break;
    default:
        return STATUS_INVALID_ARG;
    }

    s_flip_vertical   = (view->flip_vertical != FALSE) ? 1U : 0U;
    s_flip_horizontal = (view->flip_horizontal != FALSE) ? 1U : 0U;
    s_rotate_deg      = view->rotate_deg;
    return STATUS_OK;
}

status_t voice_hub_camera_view_rotate_cw(void)
{
    s_rotate_deg = (u16_t)((s_rotate_deg + 90U) % 360U);
    return STATUS_OK;
}

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

static uint32_t voice_hub_camera_strip_bytes(uint16_t row_pixels)
{
    uint32_t row_b = (uint32_t)row_pixels * 2U;
    uint32_t strip_max = (uint32_t)VOICE_HUB_CAMERA_BLIT_STRIP_BYTES_MAX;

    if (row_b == 0U) {
        return 2U;
    }

    strip_max = (strip_max / row_b) * row_b;
    if (strip_max == 0U) {
        strip_max = row_b;
    }
    return strip_max;
}

static status_t voice_hub_camera_blit_strip_ensure(uint32_t nbytes)
{
    if ((s_blit_strip != NULL) && (s_blit_strip_cap >= nbytes)) {
        return STATUS_OK;
    }

    if (s_blit_strip != NULL) {
        DmaFree(s_blit_strip);
        s_blit_strip     = NULL;
        s_blit_strip_cap = 0U;
    }

    s_blit_strip = (uint8_t *)DmaMalloc((usize_t)nbytes);
    if (s_blit_strip == NULL) {
        LOG_WARN("voice_hub camera blit strip alloc %u failed", (unsigned)nbytes);
        return STATUS_FAIL;
    }

    s_blit_strip_cap = nbytes;
    return STATUS_OK;
}

/** OV2640 输出 RGB565；可选与 ST7789 BGR 面板交换 R/B。 */
static uint16_t voice_hub_rgb565_swap_rb(uint16_t px)
{
    return (uint16_t)((px & 0x07E0U) | ((px & 0xF800U) >> 11) | ((px & 0x001FU) << 11));
}

static void voice_hub_rgb565_be_to_panel(const uint8_t *src, uint8_t *dst, uint32_t nbytes)
{
    uint32_t i;

#if !VOICE_HUB_CAMERA_PANEL_SWAP_RB
    if (src != dst) {
        (void)memcpy(dst, src, (size_t)nbytes);
    }
    return;
#endif

    for (i = 0U; i + 1U < nbytes; i += 2U) {
        uint16_t px  = (uint16_t)(((uint16_t)src[i] << 8) | (uint16_t)src[i + 1U]);
        uint16_t out = voice_hub_rgb565_swap_rb(px);

        dst[i]      = (uint8_t)(out >> 8);
        dst[i + 1U] = (uint8_t)(out & 0xFFU);
    }
}

static void voice_hub_camera_copy_row(const uint8_t *src_row, uint8_t *dst_row, uint16_t width, bool_t flip_horizontal)
{
    uint32_t row_bytes = (uint32_t)width * 2U;

    if (flip_horizontal != FALSE) {
        uint16_t x;

        for (x = 0U; x < width; x++) {
            uint16_t src_x = (uint16_t)(width - 1U - x);

            dst_row[(uint32_t)x * 2U]      = src_row[(uint32_t)src_x * 2U];
            dst_row[(uint32_t)x * 2U + 1U] = src_row[(uint32_t)src_x * 2U + 1U];
        }
        return;
    }

    (void)memcpy(dst_row, src_row, (size_t)row_bytes);
}

static void voice_hub_camera_dst_to_src(uint16_t width,
                                        uint16_t height,
                                        uint16_t dst_x,
                                        uint16_t dst_y,
                                        uint16_t *src_x,
                                        uint16_t *src_y)
{
    uint16_t x = dst_x;
    uint16_t y = dst_y;

    if (src_x == NULL || src_y == NULL) {
        return;
    }

    if (s_flip_horizontal != 0U) {
        x = (uint16_t)(width - 1U - x);
    }
    if (s_flip_vertical != 0U) {
        y = (uint16_t)(height - 1U - y);
    }

    switch (s_rotate_deg) {
    case 90U:
        *src_x = y;
        *src_y = (uint16_t)(width - 1U - x);
        break;
    case 180U:
        *src_x = (uint16_t)(width - 1U - x);
        *src_y = (uint16_t)(height - 1U - y);
        break;
    case 270U:
        *src_x = (uint16_t)(height - 1U - y);
        *src_y = x;
        break;
    default:
        *src_x = x;
        *src_y = y;
        break;
    }
}

static status_t voice_hub_camera_snapshot_mtx_init(void)
{
    if (s_snapshot_mtx != NULL) {
        return STATUS_OK;
    }
    s_snapshot_mtx = xSemaphoreCreateMutex();
    if (s_snapshot_mtx == NULL) {
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

static status_t voice_hub_camera_snapshot_buf_ensure(uint32_t nbytes)
{
    if ((s_snapshot_buf != NULL) && (s_snapshot_copy != NULL) && (s_snapshot_bytes >= nbytes)) {
        return STATUS_OK;
    }

    if (s_snapshot_buf != NULL) {
        voice_hub_camera_buf_free(s_snapshot_buf);
        s_snapshot_buf   = NULL;
        s_snapshot_bytes = 0U;
    }
    if (s_snapshot_copy != NULL) {
        voice_hub_camera_buf_free(s_snapshot_copy);
        s_snapshot_copy = NULL;
    }

    s_snapshot_buf = (uint8_t *)voice_hub_camera_buf_alloc(nbytes);
    if (s_snapshot_buf == NULL) {
        LOG_ERROR("voice_hub camera snapshot alloc %u failed", (unsigned)nbytes);
        return STATUS_FAIL;
    }

    s_snapshot_copy = (uint8_t *)voice_hub_camera_buf_alloc(nbytes);
    if (s_snapshot_copy == NULL) {
        voice_hub_camera_buf_free(s_snapshot_buf);
        s_snapshot_buf   = NULL;
        s_snapshot_bytes = 0U;
        LOG_ERROR("voice_hub camera snapshot copy alloc %u failed", (unsigned)nbytes);
        return STATUS_FAIL;
    }

    s_snapshot_bytes = nbytes;
    return STATUS_OK;
}

/** 按运行时 flip/rotate 写入 s_snapshot_buf，供 LCD / Web 共用。 */
static void voice_hub_camera_build_snapshot(const uint8_t *rgb565, uint16_t width, uint16_t height)
{
    uint32_t row_bytes;
    uint32_t frame_bytes;

    if ((rgb565 == NULL) || (width == 0U) || (height == 0U)) {
        return;
    }

    row_bytes   = (uint32_t)width * 2U;
    frame_bytes = row_bytes * (uint32_t)height;
    if (voice_hub_camera_snapshot_buf_ensure(frame_bytes) != STATUS_OK) {
        return;
    }
    if (voice_hub_camera_snapshot_mtx_init() != STATUS_OK) {
        return;
    }
    if (xSemaphoreTake(s_snapshot_mtx, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (s_rotate_deg == 0U) {
        uint16_t dst_y;

        for (dst_y = 0U; dst_y < height; dst_y++) {
            uint16_t src_y = dst_y;

            if (s_flip_vertical != 0U) {
                src_y = (uint16_t)(height - 1U - dst_y);
            }
            const uint8_t *src_row = rgb565 + ((uint32_t)src_y * row_bytes);
            uint8_t       *dst_row = s_snapshot_buf + ((uint32_t)dst_y * row_bytes);

            voice_hub_camera_copy_row(src_row,
                                      dst_row,
                                      width,
                                      (s_flip_horizontal != 0U) ? TRUE : FALSE);
        }
    } else {
        uint16_t dst_y;
        uint16_t dst_x;

        for (dst_y = 0U; dst_y < height; dst_y++) {
            for (dst_x = 0U; dst_x < width; dst_x++) {
                uint16_t src_x;
                uint16_t src_y;
                const uint8_t *sp;
                uint8_t       *dp;

                voice_hub_camera_dst_to_src(width, height, dst_x, dst_y, &src_x, &src_y);
                sp = rgb565 + ((uint32_t)src_y * row_bytes) + ((uint32_t)src_x * 2U);
                dp = s_snapshot_buf + ((uint32_t)dst_y * row_bytes) + ((uint32_t)dst_x * 2U);
                dp[0] = sp[0];
                dp[1] = sp[1];
            }
        }
    }

    s_snapshot_w = width;
    s_snapshot_h = height;
    (void)xSemaphoreGive(s_snapshot_mtx);
}

static status_t voice_hub_camera_jpeg_enc_open(void)
{
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    jpeg_error_t      err;

    if (s_jpeg_enc != NULL) {
        return STATUS_OK;
    }

    if (s_jpeg_mutex == NULL) {
        s_jpeg_mutex = xSemaphoreCreateMutex();
        if (s_jpeg_mutex == NULL) {
            return STATUS_FAIL;
        }
    }

    cfg.width      = (int)VOICE_HUB_CAMERA_PREVIEW_WIDTH;
    cfg.height     = (int)VOICE_HUB_CAMERA_PREVIEW_HEIGHT;
    cfg.src_type   = JPEG_PIXEL_FORMAT_RGB565_BE;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality    = VOICE_HUB_CAMERA_WEB_JPEG_QUALITY;
    cfg.task_enable = false;

    err = jpeg_enc_open(&cfg, &s_jpeg_enc);
    if (err != JPEG_ERR_OK) {
        LOG_WARN("voice_hub jpeg_enc_open failed %d", (int)err);
        s_jpeg_enc = NULL;
        return STATUS_FAIL;
    }

    return STATUS_OK;
}

static void voice_hub_camera_blit_rgb565(st7789_t *lcd, const uint8_t *rgb565, uint16_t cam_w, uint16_t cam_h)
{
    uint16_t lcd_w;
    uint16_t lcd_h;
    uint16_t view_h;
    uint16_t crop_y;
    uint32_t strip_max;
    uint16_t row;

    if ((lcd == NULL) || (rgb565 == NULL) || !st7789_is_initialized(lcd)) {
        return;
    }

    lcd_w = st7789_display_width(lcd);
    lcd_h = st7789_display_height(lcd);
    if (lcd_h <= VOICE_HUB_UI_IP_BAND_H) {
        return;
    }
    view_h = (uint16_t)(lcd_h - VOICE_HUB_UI_IP_BAND_H);
    if ((cam_w < lcd_w) || (cam_h < view_h)) {
        return;
    }

    strip_max = voice_hub_camera_strip_bytes(lcd_w);
    if (voice_hub_camera_blit_strip_ensure(strip_max) != STATUS_OK) {
        return;
    }

    /* rgb565 已为 FLIP 宏归一化后的显示坐标：row 0 = 画面顶部。 */
    crop_y = (uint16_t)((cam_h - view_h) / 2U);

    if (!voice_hub_ui_lcd_lock(80U)) {
        return;
    }

    if (st7789_set_window(lcd, 0U, VOICE_HUB_UI_IP_BAND_H, (uint16_t)(lcd_w - 1U), (uint16_t)(lcd_h - 1U)) !=
        ST7789_OK) {
        voice_hub_ui_lcd_unlock();
        return;
    }

    for (row = 0U; row < view_h; row++) {
        uint16_t src_row = (uint16_t)(crop_y + row);
        const uint8_t *src = rgb565 + ((uint32_t)src_row * (uint32_t)cam_w * 2U);
        uint32_t remain = (uint32_t)lcd_w * 2U;
        uint32_t offset = 0U;

        while (remain > 0U) {
            uint32_t chunk = (remain > strip_max) ? strip_max : remain;

            voice_hub_rgb565_be_to_panel(src + offset, s_blit_strip, chunk);
            if (st7789_write_pixel_bytes(lcd, s_blit_strip, chunk) != ST7789_OK) {
                st7789_end_write(lcd);
                voice_hub_ui_lcd_unlock();
                return;
            }
            offset += chunk;
            remain -= chunk;
        }
    }

    st7789_end_write(lcd);
    voice_hub_ui_lcd_unlock();
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

        if (s_preview_frame == NULL) {
            continue;
        }

        voice_hub_camera_build_snapshot((const uint8_t *)s_preview_frame, s_preview_frame_w, s_preview_frame_h);

#if VOICE_HUB_ENABLE_LCD
        if ((lcd != NULL) && st7789_is_initialized(lcd) && (s_snapshot_buf != NULL)) {
            voice_hub_camera_blit_rgb565(lcd, s_snapshot_buf, s_snapshot_w, s_snapshot_h);
        }
#endif
    }
}

static status_t voice_hub_camera_preview_task_start(st7789_t *lcd)
{
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

    if (voice_hub_camera_snapshot_buf_ensure(VOICE_HUB_CAMERA_SNAPSHOT_BYTES) != STATUS_OK) {
        LOG_ERROR("voice_hub camera snapshot buffer init failed");
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

    if (voice_hub_camera_preview_task_start(lcd) != STATUS_OK) {
        return STATUS_FAIL;
    }

#if VOICE_HUB_ENABLE_LCD
    if ((lcd != NULL) && st7789_is_initialized(lcd)) {
        if (voice_hub_camera_blit_strip_ensure(voice_hub_camera_strip_bytes(st7789_display_width(lcd))) != STATUS_OK) {
            LOG_WARN("voice_hub camera blit strip init failed (LCD preview disabled)");
        }
    }
#endif

    if (ov2640_set_frame_callback(&s_ov2640, voice_hub_camera_on_frame, NULL) != OV2640_OK) {
        LOG_WARN("ov2640_set_frame_callback failed");
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

status_t voice_hub_camera_snapshot_jpeg(uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
#if !VOICE_HUB_ENABLE_CAMERA
    (void)out;
    (void)out_cap;
    if (out_len != NULL) {
        *out_len = 0U;
    }
    return STATUS_FAIL;
#else
    uint32_t  frame_bytes;
    uint16_t  width;
    uint16_t  height;
    int       jpeg_size = 0;
    jpeg_error_t jerr;

    if ((out == NULL) || (out_cap == 0U) || (out_len == NULL) || !voice_hub_camera_is_ready()) {
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return STATUS_INVALID_ARG;
    }

    *out_len = 0U;

    if (voice_hub_camera_snapshot_mtx_init() != STATUS_OK) {
        return STATUS_FAIL;
    }
    if (xSemaphoreTake(s_snapshot_mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        return STATUS_FAIL;
    }

    width       = s_snapshot_w;
    height      = s_snapshot_h;
    frame_bytes = (uint32_t)width * (uint32_t)height * 2U;
    if ((s_snapshot_buf == NULL) || (s_snapshot_copy == NULL) || (frame_bytes == 0U) ||
        (frame_bytes > s_snapshot_bytes)) {
        (void)xSemaphoreGive(s_snapshot_mtx);
        return STATUS_FAIL;
    }

    (void)memcpy(s_snapshot_copy, s_snapshot_buf, (size_t)frame_bytes);
    (void)xSemaphoreGive(s_snapshot_mtx);

    if (voice_hub_camera_jpeg_enc_open() != STATUS_OK) {
        return STATUS_FAIL;
    }

    if (xSemaphoreTake(s_jpeg_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return STATUS_FAIL;
    }

    jerr = jpeg_enc_process(s_jpeg_enc,
                            s_snapshot_copy,
                            (int)frame_bytes,
                            out,
                            (int)out_cap,
                            &jpeg_size);
    (void)xSemaphoreGive(s_jpeg_mutex);

    if ((jerr != JPEG_ERR_OK) || (jpeg_size <= 0)) {
        LOG_WARN("voice_hub jpeg_enc_process failed %d", (int)jerr);
        return STATUS_FAIL;
    }

    *out_len = (uint32_t)jpeg_size;
    return STATUS_OK;
#endif
}

void voice_hub_camera_prepare_for_reboot(void)
{
#if VOICE_HUB_ENABLE_CAMERA
    if (s_jpeg_mutex != NULL) {
        (void)xSemaphoreTake(s_jpeg_mutex, pdMS_TO_TICKS(500));
        (void)xSemaphoreGive(s_jpeg_mutex);
    }
#endif
}
