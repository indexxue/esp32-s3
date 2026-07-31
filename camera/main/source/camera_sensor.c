#include "camera_sensor.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#include "board.h"
#include "camera_ui.h"
#include "camera_model.h"

#include "dma.h"
#include "i2c.h"
#include "driver/i2c_master.h"
#include "lcd.h"
#include "log.h"
#include "ov2640.h"

#include "esp_jpeg_enc.h"

static ov2640_t s_ov2640;
static bool_t s_camera_ready;
static st7789_t *s_preview_lcd;
static TaskHandle_t s_preview_task;
static TaskHandle_t s_jpeg_task;
static volatile const uint8_t *s_preview_frame;
static volatile uint16_t s_preview_frame_w;
static volatile uint16_t s_preview_frame_h;

#define CAMERA_SENSOR_PREVIEW_TASK_STACK (8192U)
#define CAMERA_SENSOR_PREVIEW_TASK_PRIO (5U)
#define CAMERA_SENSOR_JPEG_TASK_STACK (6144U)
#define CAMERA_SENSOR_JPEG_TASK_PRIO (4U)
#define CAMERA_SENSOR_LCD_LOCK_MS (200U)
#define CAMERA_SENSOR_SNAP_SLOT_COUNT (2U)
#define CAMERA_SENSOR_DETECT_HOLD_NONE (0xFFU)

/** 双槽 snapshot：预览写一槽，检测可持有另一槽。 */
static uint8_t *s_snap_slot[CAMERA_SENSOR_SNAP_SLOT_COUNT];
static uint8_t s_snap_pub;
static uint8_t s_snap_detect_hold = CAMERA_SENSOR_DETECT_HOLD_NONE;
static uint32_t s_snap_gen;
static uint8_t *s_snapshot_copy;
static uint32_t s_snapshot_bytes;
static uint16_t s_snapshot_w;
static uint16_t s_snapshot_h;
static SemaphoreHandle_t s_snapshot_mtx;
static SemaphoreHandle_t s_jpeg_mutex;
static jpeg_enc_handle_t s_jpeg_enc;
static uint8_t *s_blit_strip;
static uint32_t s_blit_strip_cap;
static volatile uint8_t s_flip_vertical   = (uint8_t)CAMERA_SENSOR_FLIP_VERTICAL;
static volatile uint8_t s_flip_horizontal = (uint8_t)CAMERA_SENSOR_FLIP_HORIZONTAL;
static volatile uint8_t s_jpeg_busy        = 0U;
static TickType_t       s_last_snapshot_tick;
static TickType_t       s_last_jpeg_tick;
static TickType_t       s_last_lcd_blit_tick;
static volatile uint32_t s_frame_count;
static volatile uint32_t s_lcd_blit_count;

static uint16_t s_web_w = (uint16_t)CAMERA_SENSOR_WEB_DEFAULT_WIDTH;
static uint16_t s_web_h = (uint16_t)CAMERA_SENSOR_WEB_DEFAULT_HEIGHT;
static uint8_t  s_jpeg_quality = (uint8_t)CAMERA_SENSOR_WEB_JPEG_QUALITY;
static uint8_t  s_grayscale    = 0U;
static uint8_t  s_zoom         = 1U;
static uint8_t  s_img_rotate   = 0U; /* 0/1/2/3 → 0°/90°/180°/270° */
static uint16_t s_jpeg_enc_w;
static uint16_t s_jpeg_enc_h;
static uint8_t  s_jpeg_enc_q;

static uint8_t *s_jpeg_cache;
static uint32_t s_jpeg_cache_len;
static SemaphoreHandle_t s_jpeg_cache_mtx;
static volatile uint32_t s_jpeg_seq;
static SemaphoreHandle_t s_jpeg_seq_sem;
static volatile uint32_t s_web_stream_clients;
/** 因独占预览主动关过 LCD，leave 到 0 时需 open 回来。 */
static bool_t s_lcd_held_for_stream;

static void *camera_sensor_buf_alloc(uint32_t size)
{
    void *p;

    p = heap_caps_aligned_alloc(16, (size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_aligned_alloc(16, (size_t)size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void camera_sensor_buf_free(void *ptr)
{
    if (ptr != NULL) {
        heap_caps_free(ptr);
    }
}

static int ov2640_sccb_write(uint8_t addr7, const uint8_t *data, uint16_t len)
{
    if (I2cWrite((s32_t)BOARD_I2C_OV2640_SCCB_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int ov2640_sccb_read(uint8_t addr7, uint8_t *data, uint16_t len)
{
    if (I2cRead((s32_t)BOARD_I2C_OV2640_SCCB_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int ov2640_sccb_write_read(uint8_t addr7,
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

static ov2640_pin_config_t camera_sensor_pin_config(void)
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

static uint32_t camera_sensor_strip_bytes(uint16_t row_pixels)
{
    uint32_t row_b = (uint32_t)row_pixels * 2U;
    uint32_t strip_max = (uint32_t)CAMERA_SENSOR_BLIT_STRIP_BYTES_MAX;

    if (row_b == 0U) {
        return 2U;
    }

    strip_max = (strip_max / row_b) * row_b;
    if (strip_max == 0U) {
        strip_max = row_b;
    }
    return strip_max;
}

static status_t camera_sensor_blit_strip_ensure(uint32_t nbytes)
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
        LOG_WARN("camera sensor blit strip alloc %u failed", (unsigned)nbytes);
        return STATUS_FAIL;
    }

    s_blit_strip_cap = nbytes;
    return STATUS_OK;
}

/** OV2640 输出 RGB565；可选与 ST7789 BGR 面板交换 R/B。 */
static uint16_t rgb565_swap_rb(uint16_t px)
{
    return (uint16_t)((px & 0x07E0U) | ((px & 0xF800U) >> 11) | ((px & 0x001FU) << 11));
}

static void rgb565_be_to_panel(const uint8_t *src, uint8_t *dst, uint32_t nbytes)
{
    uint32_t i;

#if !CAMERA_SENSOR_PANEL_SWAP_RB
    if (src != dst) {
        (void)memcpy(dst, src, (size_t)nbytes);
    }
    return;
#endif

    for (i = 0U; i + 1U < nbytes; i += 2U) {
        uint16_t px  = (uint16_t)(((uint16_t)src[i] << 8) | (uint16_t)src[i + 1U]);
        uint16_t out = rgb565_swap_rb(px);

        dst[i]      = (uint8_t)(out >> 8);
        dst[i + 1U] = (uint8_t)(out & 0xFFU);
    }
}

static status_t camera_sensor_snapshot_mtx_init(void)
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

static status_t camera_sensor_snapshot_buf_ensure(uint32_t nbytes)
{
    uint8_t i;

    if ((s_snap_slot[0] != NULL) && (s_snap_slot[1] != NULL) && (s_snapshot_copy != NULL) &&
        (s_snapshot_bytes >= nbytes)) {
        return STATUS_OK;
    }

    for (i = 0U; i < CAMERA_SENSOR_SNAP_SLOT_COUNT; i++) {
        if (s_snap_slot[i] != NULL) {
            camera_sensor_buf_free(s_snap_slot[i]);
            s_snap_slot[i] = NULL;
        }
    }
    if (s_snapshot_copy != NULL) {
        camera_sensor_buf_free(s_snapshot_copy);
        s_snapshot_copy = NULL;
    }
    s_snapshot_bytes = 0U;

    for (i = 0U; i < CAMERA_SENSOR_SNAP_SLOT_COUNT; i++) {
        s_snap_slot[i] = (uint8_t *)camera_sensor_buf_alloc(nbytes);
        if (s_snap_slot[i] == NULL) {
            LOG_ERROR("camera sensor snapshot slot%u alloc %u failed", (unsigned)i, (unsigned)nbytes);
            for (uint8_t j = 0U; j < i; j++) {
                camera_sensor_buf_free(s_snap_slot[j]);
                s_snap_slot[j] = NULL;
            }
            return STATUS_FAIL;
        }
    }

    s_snapshot_copy = (uint8_t *)camera_sensor_buf_alloc(nbytes);
    if (s_snapshot_copy == NULL) {
        for (i = 0U; i < CAMERA_SENSOR_SNAP_SLOT_COUNT; i++) {
            camera_sensor_buf_free(s_snap_slot[i]);
            s_snap_slot[i] = NULL;
        }
        LOG_ERROR("camera sensor snapshot copy alloc %u failed", (unsigned)nbytes);
        return STATUS_FAIL;
    }

    s_snapshot_bytes = nbytes;
    s_snap_pub = 0U;
    s_snap_detect_hold = CAMERA_SENSOR_DETECT_HOLD_NONE;
    return STATUS_OK;
}

static uint8_t camera_sensor_pick_write_slot(void)
{
    if (s_snap_detect_hold < CAMERA_SENSOR_SNAP_SLOT_COUNT) {
        return (uint8_t)(s_snap_detect_hold ^ 1U);
    }
    return (uint8_t)(s_snap_pub ^ 1U);
}

static uint8_t *camera_sensor_pub_buf(void)
{
    return s_snap_slot[s_snap_pub & 1U];
}

/** 按运行时 flip/zoom/rotate/灰度，从采集帧写入双槽之一并发布。 */
static void camera_sensor_build_snapshot(const uint8_t *rgb565, uint16_t width, uint16_t height)
{
    uint16_t pre_w;
    uint16_t pre_h;
    uint16_t fin_w;
    uint16_t fin_h;
    uint16_t src_cw;
    uint16_t src_ch;
    uint16_t x0;
    uint16_t y0;
    uint8_t  zoom;
    uint32_t src_row_bytes;
    uint32_t pre_bytes;
    uint32_t fin_bytes;
    uint16_t py;
    uint16_t px;
    uint8_t  write_slot;
    uint8_t *write_buf;

    pre_w = s_web_w;
    pre_h = s_web_h;
    zoom  = s_zoom;
    if (zoom < 1U) {
        zoom = 1U;
    }
    if ((rgb565 == NULL) || (pre_w == 0U) || (pre_h == 0U) || (width < zoom) || (height < zoom)) {
        return;
    }

    /*
     * 变焦：从采集帧中心取 (W/zoom)×(H/zoom)，再按预览宽高比裁切后缩放到 pre_w×pre_h。
     * zoom=1 → 整幅缩小/等比例；zoom=2 → 半幅视野，以此类推。
     */
    {
        uint16_t max_w = (uint16_t)(width / zoom);
        uint16_t max_h = (uint16_t)(height / zoom);

        if (((uint32_t)max_w * (uint32_t)pre_h) > ((uint32_t)max_h * (uint32_t)pre_w)) {
            src_ch = max_h;
            src_cw = (uint16_t)(((uint32_t)max_h * (uint32_t)pre_w) / (uint32_t)pre_h);
        } else {
            src_cw = max_w;
            src_ch = (uint16_t)(((uint32_t)max_w * (uint32_t)pre_h) / (uint32_t)pre_w);
        }
    }
    if ((src_cw == 0U) || (src_ch == 0U) || (width < src_cw) || (height < src_ch)) {
        return;
    }

    if ((s_img_rotate == 1U) || (s_img_rotate == 3U)) {
        fin_w = pre_h;
        fin_h = pre_w;
    } else {
        fin_w = pre_w;
        fin_h = pre_h;
    }

    src_row_bytes = (uint32_t)width * 2U;
    pre_bytes     = (uint32_t)pre_w * (uint32_t)pre_h * 2U;
    fin_bytes     = (uint32_t)fin_w * (uint32_t)fin_h * 2U;
    x0            = (uint16_t)((width - src_cw) / 2U);
    y0            = (uint16_t)((height - src_ch) / 2U);

    if (camera_sensor_snapshot_buf_ensure(
            (pre_bytes > fin_bytes) ? pre_bytes : fin_bytes) != STATUS_OK) {
        return;
    }
    if (camera_sensor_snapshot_mtx_init() != STATUS_OK) {
        return;
    }
    if (xSemaphoreTake(s_snapshot_mtx, portMAX_DELAY) != pdTRUE) {
        return;
    }

    write_slot = camera_sensor_pick_write_slot();
    write_buf  = s_snap_slot[write_slot];
    if (write_buf == NULL) {
        (void)xSemaphoreGive(s_snapshot_mtx);
        return;
    }

    /* 快路径：1:1、无旋转/灰度时只做翻转 memcpy。 */
    if ((s_img_rotate == 0U) && (s_grayscale == 0U) && (zoom == 1U) && (src_cw == pre_w) &&
        (src_ch == pre_h) && (width == pre_w) && (height == pre_h)) {
        if ((s_flip_vertical == 0U) && (s_flip_horizontal == 0U)) {
            (void)memcpy(write_buf, rgb565, (size_t)fin_bytes);
        } else if ((s_flip_vertical != 0U) && (s_flip_horizontal == 0U)) {
            for (py = 0U; py < pre_h; py++) {
                const uint8_t *sp = rgb565 + ((uint32_t)(pre_h - 1U - py) * src_row_bytes);
                uint8_t *dp = write_buf + ((uint32_t)py * (uint32_t)pre_w * 2U);
                (void)memcpy(dp, sp, (size_t)((uint32_t)pre_w * 2U));
            }
        } else if ((s_flip_vertical == 0U) && (s_flip_horizontal != 0U)) {
            for (py = 0U; py < pre_h; py++) {
                const uint8_t *sp = rgb565 + ((uint32_t)py * src_row_bytes);
                uint8_t *dp = write_buf + ((uint32_t)py * (uint32_t)pre_w * 2U);
                for (px = 0U; px < pre_w; px++) {
                    const uint8_t *s = sp + ((uint32_t)(pre_w - 1U - px) * 2U);
                    uint8_t *d = dp + ((uint32_t)px * 2U);
                    d[0] = s[0];
                    d[1] = s[1];
                }
            }
        } else {
            for (py = 0U; py < pre_h; py++) {
                const uint8_t *sp = rgb565 + ((uint32_t)(pre_h - 1U - py) * src_row_bytes);
                uint8_t *dp = write_buf + ((uint32_t)py * (uint32_t)pre_w * 2U);
                for (px = 0U; px < pre_w; px++) {
                    const uint8_t *s = sp + ((uint32_t)(pre_w - 1U - px) * 2U);
                    uint8_t *d = dp + ((uint32_t)px * 2U);
                    d[0] = s[0];
                    d[1] = s[1];
                }
            }
        }
        s_snapshot_w = fin_w;
        s_snapshot_h = fin_h;
        s_snap_pub   = write_slot;
        s_snap_gen++;
        (void)xSemaphoreGive(s_snapshot_mtx);
        return;
    }

    /*
     * rotate=0：缩放+翻转直接写入 write 槽。
     * rotate≠0：先写入 copy，再旋转到 write 槽。
     */
    {
        uint8_t *scale_dst = (s_img_rotate == 0U) ? write_buf : s_snapshot_copy;
        bool_t   int_step =
            (((src_cw % pre_w) == 0U) && ((src_ch % pre_h) == 0U)) ? TRUE : FALSE;
        uint16_t step_x = int_step ? (uint16_t)(src_cw / pre_w) : 0U;
        uint16_t step_y = int_step ? (uint16_t)(src_ch / pre_h) : 0U;

        for (py = 0U; py < pre_h; py++) {
            uint16_t sy_idx = (s_flip_vertical != 0U) ? (uint16_t)(pre_h - 1U - py) : py;
            uint16_t sy =
                int_step ? (uint16_t)(y0 + ((uint32_t)sy_idx * (uint32_t)step_y))
                         : (uint16_t)(y0 + ((uint32_t)sy_idx * (uint32_t)src_ch) / (uint32_t)pre_h);
            uint8_t *dp_row = scale_dst + ((uint32_t)py * (uint32_t)pre_w * 2U);

            for (px = 0U; px < pre_w; px++) {
                uint16_t sx_idx = (s_flip_horizontal != 0U) ? (uint16_t)(pre_w - 1U - px) : px;
                uint16_t sx =
                    int_step ? (uint16_t)(x0 + ((uint32_t)sx_idx * (uint32_t)step_x))
                             : (uint16_t)(x0 + ((uint32_t)sx_idx * (uint32_t)src_cw) /
                                                   (uint32_t)pre_w);
                const uint8_t *sp = rgb565 + ((uint32_t)sy * src_row_bytes) + ((uint32_t)sx * 2U);
                uint8_t       *dp = dp_row + ((uint32_t)px * 2U);

                dp[0] = sp[0];
                dp[1] = sp[1];
            }
        }
    }

    if (s_img_rotate != 0U) {
        for (py = 0U; py < fin_h; py++) {
            for (px = 0U; px < fin_w; px++) {
                uint16_t sx;
                uint16_t sy;
                const uint8_t *sp;
                uint8_t       *dp;

                switch (s_img_rotate) {
                case 1U: /* 90° CW */
                    sx = py;
                    sy = (uint16_t)(pre_h - 1U - px);
                    break;
                case 2U: /* 180° */
                    sx = (uint16_t)(pre_w - 1U - px);
                    sy = (uint16_t)(pre_h - 1U - py);
                    break;
                case 3U: /* 270° CW */
                    sx = (uint16_t)(pre_w - 1U - py);
                    sy = px;
                    break;
                default:
                    sx = px;
                    sy = py;
                    break;
                }
                sp = s_snapshot_copy + ((uint32_t)sy * (uint32_t)pre_w * 2U) + ((uint32_t)sx * 2U);
                dp = write_buf + ((uint32_t)py * (uint32_t)fin_w * 2U) + ((uint32_t)px * 2U);
                dp[0] = sp[0];
                dp[1] = sp[1];
            }
        }
    }

    if (s_grayscale != 0U) {
        uint32_t i;

        for (i = 0U; i + 1U < fin_bytes; i += 2U) {
            uint16_t pxv = (uint16_t)(((uint16_t)write_buf[i] << 8) | (uint16_t)write_buf[i + 1U]);
            uint32_t r   = ((uint32_t)(pxv >> 11) & 0x1FU) * 255U / 31U;
            uint32_t g   = ((uint32_t)(pxv >> 5) & 0x3FU) * 255U / 63U;
            uint32_t b   = ((uint32_t)(pxv) & 0x1FU) * 255U / 31U;
            uint32_t y   = (77U * r + 150U * g + 29U * b) >> 8;
            uint16_t out = (uint16_t)(((y >> 3) << 11) | ((y >> 2) << 5) | (y >> 3));

            write_buf[i]      = (uint8_t)(out >> 8);
            write_buf[i + 1U] = (uint8_t)(out & 0xFFU);
        }
    }

    s_snapshot_w = fin_w;
    s_snapshot_h = fin_h;
    s_snap_pub   = write_slot;
    s_snap_gen++;
    (void)xSemaphoreGive(s_snapshot_mtx);
}

static void camera_sensor_jpeg_enc_close(void)
{
    if (s_jpeg_enc != NULL) {
        (void)jpeg_enc_close(s_jpeg_enc);
        s_jpeg_enc = NULL;
    }
    s_jpeg_enc_w = 0U;
    s_jpeg_enc_h = 0U;
    s_jpeg_enc_q = 0U;
}

static status_t camera_sensor_jpeg_enc_open(void)
{
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    jpeg_error_t      err;
    uint16_t          want_w;
    uint16_t          want_h;
    uint8_t           want_q = s_jpeg_quality;

    /* 编码器按最终快照尺寸（含 90/270 对调）打开。 */
    if ((s_img_rotate == 1U) || (s_img_rotate == 3U)) {
        want_w = s_web_h;
        want_h = s_web_w;
    } else {
        want_w = s_web_w;
        want_h = s_web_h;
    }

    if (s_jpeg_mutex == NULL) {
        s_jpeg_mutex = xSemaphoreCreateMutex();
        if (s_jpeg_mutex == NULL) {
            return STATUS_FAIL;
        }
    }

    if ((s_jpeg_enc != NULL) && (s_jpeg_enc_w == want_w) && (s_jpeg_enc_h == want_h) &&
        (s_jpeg_enc_q == want_q)) {
        return STATUS_OK;
    }

    camera_sensor_jpeg_enc_close();

    cfg.width       = (int)want_w;
    cfg.height      = (int)want_h;
    cfg.src_type    = JPEG_PIXEL_FORMAT_RGB565_BE;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality     = (int)want_q;
    cfg.task_enable = false;

    err = jpeg_enc_open(&cfg, &s_jpeg_enc);
    if (err != JPEG_ERR_OK) {
        LOG_WARN("camera jpeg_enc_open failed %d", (int)err);
        s_jpeg_enc = NULL;
        return STATUS_FAIL;
    }

    s_jpeg_enc_w = want_w;
    s_jpeg_enc_h = want_h;
    s_jpeg_enc_q = want_q;
    return STATUS_OK;
}

static status_t camera_sensor_jpeg_cache_ensure(void)
{
    if (s_jpeg_cache != NULL) {
        if (s_jpeg_seq_sem == NULL) {
            s_jpeg_seq_sem = xSemaphoreCreateBinary();
            if (s_jpeg_seq_sem == NULL) {
                return STATUS_FAIL;
            }
        }
        return STATUS_OK;
    }
    s_jpeg_cache = (uint8_t *)camera_sensor_buf_alloc(CAMERA_SENSOR_JPEG_CACHE_CAP);
    if (s_jpeg_cache == NULL) {
        return STATUS_FAIL;
    }
    if (s_jpeg_cache_mtx == NULL) {
        s_jpeg_cache_mtx = xSemaphoreCreateMutex();
        if (s_jpeg_cache_mtx == NULL) {
            camera_sensor_buf_free(s_jpeg_cache);
            s_jpeg_cache = NULL;
            return STATUS_FAIL;
        }
    }
    if (s_jpeg_seq_sem == NULL) {
        s_jpeg_seq_sem = xSemaphoreCreateBinary();
        if (s_jpeg_seq_sem == NULL) {
            return STATUS_FAIL;
        }
    }
    return STATUS_OK;
}

#if CAMERA_DETECT_OVERLAY_WEB
/** 在 JPEG 编码前的 RGB565 拷贝上叠检测框（不改 snapshot 主缓冲）。 */
static void camera_sensor_overlay_detect_on_rgb565(uint8_t *rgb565, uint16_t width, uint16_t height)
{
    uint16_t box_flat[CAMERA_MODEL_MAX_BOXES * CAMERA_MODEL_UI_BOX_STRIDE];
    uint8_t  box_count = 0U;
    uint16_t box_w = width;
    uint16_t box_h = height;

    if ((rgb565 == NULL) || (width == 0U) || (height == 0U)) {
        return;
    }
    if (camera_model_is_ready() == FALSE) {
        return;
    }
    if (camera_model_fill_ui_boxes(box_flat,
                                   (uint8_t)CAMERA_MODEL_MAX_BOXES,
                                   &box_count,
                                   &box_w,
                                   &box_h) != STATUS_OK) {
        return;
    }
    if (box_count == 0U) {
        return;
    }
    if (box_w == 0U) {
        box_w = width;
    }
    if (box_h == 0U) {
        box_h = height;
    }
    camera_ui_draw_boxes_rgb565(rgb565, width, height, box_flat, box_count, box_w, box_h);
}
#endif

/** 将当前 snapshot 预编码进 cache，供网页快速取帧。 */
static void camera_sensor_refresh_jpeg_cache(void)
{
    uint32_t     frame_bytes;
    uint16_t     width;
    uint16_t     height;
    int          jpeg_size = 0;
    jpeg_error_t jerr;

    if (camera_sensor_jpeg_cache_ensure() != STATUS_OK) {
        return;
    }
    if (camera_sensor_snapshot_mtx_init() != STATUS_OK) {
        return;
    }
    if (xSemaphoreTake(s_snapshot_mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    width       = s_snapshot_w;
    height      = s_snapshot_h;
    frame_bytes = (uint32_t)width * (uint32_t)height * 2U;
    {
        uint8_t *pub = camera_sensor_pub_buf();

        if ((pub == NULL) || (s_snapshot_copy == NULL) || (frame_bytes == 0U) ||
            (frame_bytes > s_snapshot_bytes) || (width == 0U) || (height == 0U)) {
            (void)xSemaphoreGive(s_snapshot_mtx);
            return;
        }
        (void)memcpy(s_snapshot_copy, pub, (size_t)frame_bytes);
    }
    (void)xSemaphoreGive(s_snapshot_mtx);

#if CAMERA_DETECT_OVERLAY_WEB
    camera_sensor_overlay_detect_on_rgb565(s_snapshot_copy, width, height);
#endif

    if (camera_sensor_jpeg_enc_open() != STATUS_OK) {
        return;
    }
    if (xSemaphoreTake(s_jpeg_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    s_jpeg_busy = 1U;
    jerr = jpeg_enc_process(s_jpeg_enc,
                            s_snapshot_copy,
                            (int)frame_bytes,
                            s_jpeg_cache,
                            (int)CAMERA_SENSOR_JPEG_CACHE_CAP,
                            &jpeg_size);
    s_jpeg_busy = 0U;
    (void)xSemaphoreGive(s_jpeg_mutex);

    if ((jerr != JPEG_ERR_OK) || (jpeg_size <= 0)) {
        return;
    }

    if (xSemaphoreTake(s_jpeg_cache_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_jpeg_cache_len = (uint32_t)jpeg_size;
        s_jpeg_seq++;
        (void)xSemaphoreGive(s_jpeg_cache_mtx);
        if (s_jpeg_seq_sem != NULL) {
            (void)xSemaphoreGive(s_jpeg_seq_sem);
        }
    }
}

static void camera_sensor_blit_rgb565(st7789_t *lcd, const uint8_t *rgb565, uint16_t cam_w, uint16_t cam_h)
{
    uint16_t lcd_w;
    uint16_t lcd_h;
    uint16_t view_h;
    uint16_t crop_x;
    uint16_t crop_y;
    uint32_t strip_max;
    uint16_t row;

    if ((lcd == NULL) || (rgb565 == NULL) || !st7789_is_initialized(lcd)) {
        return;
    }

    lcd_w = st7789_display_width(lcd);
    lcd_h = st7789_display_height(lcd);
    if (lcd_h <= CAMERA_UI_IP_BAND_H) {
        return;
    }
    view_h = (uint16_t)(lcd_h - CAMERA_UI_IP_BAND_H);
    if ((cam_w < lcd_w) || (cam_h < view_h)) {
        return;
    }

    strip_max = camera_sensor_strip_bytes(lcd_w);
    if (camera_sensor_blit_strip_ensure(strip_max) != STATUS_OK) {
        return;
    }

    /* rgb565 已为 FLIP 宏归一化后的显示坐标：row 0 = 画面顶部；水平也居中裁。 */
    crop_x = (uint16_t)((cam_w - lcd_w) / 2U);
    crop_y = (uint16_t)((cam_h - view_h) / 2U);

    if (!camera_ui_lcd_lock(CAMERA_SENSOR_LCD_LOCK_MS)) {
        return;
    }

    if (st7789_set_window(lcd, 0U, CAMERA_UI_IP_BAND_H, (uint16_t)(lcd_w - 1U), (uint16_t)(lcd_h - 1U)) !=
        ST7789_OK) {
        camera_ui_lcd_unlock();
        return;
    }

    for (row = 0U; row < view_h; row++) {
        uint16_t src_row = (uint16_t)(crop_y + row);
        const uint8_t *src =
            rgb565 + ((uint32_t)src_row * (uint32_t)cam_w * 2U) + ((uint32_t)crop_x * 2U);
        uint32_t remain = (uint32_t)lcd_w * 2U;
        uint32_t offset = 0U;

        while (remain > 0U) {
            uint32_t chunk = (remain > strip_max) ? strip_max : remain;

            rgb565_be_to_panel(src + offset, s_blit_strip, chunk);
            if (st7789_write_pixel_bytes(lcd, s_blit_strip, chunk) != ST7789_OK) {
                st7789_end_write(lcd);
                camera_ui_lcd_unlock();
                return;
            }
            offset += chunk;
            remain -= chunk;
        }
    }

    st7789_end_write(lcd);
    camera_ui_lcd_unlock();
}

static void camera_sensor_on_frame(void *user_ctx, const uint8_t *rgb565, uint16_t width, uint16_t height)
{
    BaseType_t wake = pdFALSE;

    (void)user_ctx;
    s_preview_frame   = rgb565;
    s_preview_frame_w = width;
    s_preview_frame_h = height;
    s_frame_count++;

    if (s_preview_task != NULL) {
        (void)vTaskNotifyGiveFromISR(s_preview_task, &wake);
        portYIELD_FROM_ISR(wake);
    }
}

static void camera_sensor_jpeg_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        camera_sensor_refresh_jpeg_cache();
    }
}

static status_t camera_sensor_jpeg_task_start(void)
{
    if (s_jpeg_task != NULL) {
        return STATUS_OK;
    }
    if (xTaskCreate(camera_sensor_jpeg_task,
                    "cam_jpeg",
                    CAMERA_SENSOR_JPEG_TASK_STACK,
                    NULL,
                    CAMERA_SENSOR_JPEG_TASK_PRIO,
                    &s_jpeg_task) != pdPASS) {
        s_jpeg_task = NULL;
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

static void camera_sensor_jpeg_request(void)
{
    if (s_jpeg_task != NULL) {
        (void)xTaskNotifyGive(s_jpeg_task);
    } else {
        camera_sensor_refresh_jpeg_cache();
    }
}

static void camera_sensor_preview_task(void *arg)
{
    st7789_t *lcd = (st7789_t *)arg;

    for (;;) {
        TickType_t now;
        TickType_t since_snap;
        TickType_t since_lcd;
        TickType_t since_jpeg;
        TickType_t snap_min;
        TickType_t jpeg_min;
        TickType_t lcd_min;
        bool_t     do_snap;
        bool_t     do_lcd;
        bool_t     do_jpeg;
        uint8_t   *pub;

        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s_preview_frame == NULL) {
            continue;
        }

        now        = xTaskGetTickCount();
        since_snap = now - s_last_snapshot_tick;
        since_lcd  = now - s_last_lcd_blit_tick;
        since_jpeg = now - s_last_jpeg_tick;
        lcd_min    = pdMS_TO_TICKS((s_web_stream_clients > 0U) ? CAMERA_SENSOR_LCD_STREAM_INTERVAL_MS
                                                               : CAMERA_SENSOR_LCD_MIN_INTERVAL_MS);
        /* MJPEG 在线：~15fps 图传；空闲：snapshot 仍 25fps 供检测，JPEG 降到 5fps。 */
        if (s_web_stream_clients > 0U) {
            snap_min = pdMS_TO_TICKS(CAMERA_SENSOR_SNAPSHOT_STREAM_INTERVAL_MS);
            jpeg_min = snap_min;
        } else {
            snap_min = pdMS_TO_TICKS(CAMERA_SENSOR_SNAPSHOT_MIN_INTERVAL_MS);
            jpeg_min = pdMS_TO_TICKS(CAMERA_SENSOR_JPEG_IDLE_INTERVAL_MS);
        }
        do_snap = ((s_last_snapshot_tick == 0U) || (since_snap >= snap_min)) ? TRUE : FALSE;
        do_lcd  = ((s_last_lcd_blit_tick == 0U) || (since_lcd >= lcd_min)) ? TRUE : FALSE;
        do_jpeg = ((s_last_jpeg_tick == 0U) || (since_jpeg >= jpeg_min)) ? TRUE : FALSE;

        /* 独占预览：彻底停 LCD blit（stream_enter 已关背光；此处双保险）。 */
        if (s_web_stream_clients > 0U) {
            do_lcd = FALSE;
        }

        /* JPEG 编码期间跳过本轮 LCD blit，避免 SPI 与编码叠加重导致锁超时丢帧。 */
        if (s_jpeg_busy != 0U) {
            do_lcd = FALSE;
        }

        if ((do_snap == FALSE) && (do_lcd == FALSE) && (do_jpeg == FALSE)) {
            continue;
        }

        if (do_snap != FALSE) {
            camera_sensor_build_snapshot((const uint8_t *)s_preview_frame,
                                        s_preview_frame_w,
                                        s_preview_frame_h);
            s_last_snapshot_tick = xTaskGetTickCount();
        }

        pub = camera_sensor_pub_buf();
        if ((do_lcd != FALSE) && (lcd != NULL) && st7789_is_initialized(lcd) &&
            (camera_ui_lcd_is_open() != FALSE) && (pub != NULL) &&
            (s_snapshot_w > 0U) && (s_snapshot_h > 0U)) {
            camera_sensor_blit_rgb565(lcd, pub, s_snapshot_w, s_snapshot_h);
#if CAMERA_DETECT_OVERLAY_LCD
            if (camera_model_is_ready() != FALSE) {
                uint16_t box_flat[CAMERA_MODEL_MAX_BOXES * CAMERA_MODEL_UI_BOX_STRIDE];
                uint8_t box_count = 0U;
                uint16_t box_w = s_snapshot_w;
                uint16_t box_h = s_snapshot_h;

                if (camera_model_fill_ui_boxes(box_flat,
                                               (uint8_t)CAMERA_MODEL_MAX_BOXES,
                                               &box_count,
                                               &box_w,
                                               &box_h) == STATUS_OK) {
                    if (box_count > 0U) {
                        if (box_w == 0U) {
                            box_w = s_snapshot_w;
                        }
                        if (box_h == 0U) {
                            box_h = s_snapshot_h;
                        }
                        camera_ui_draw_detection_boxes(lcd, box_flat, box_count, box_w, box_h);
                    }
                }
            }
#endif
            s_last_lcd_blit_tick = xTaskGetTickCount();
            s_lcd_blit_count++;
            if ((s_lcd_blit_count == 1U) || ((s_lcd_blit_count % 50U) == 0U)) {
                LOG_INFO("cam preview frames=%u lcd_blit=%u %ux%u",
                         (unsigned)s_frame_count,
                         (unsigned)s_lcd_blit_count,
                         (unsigned)s_snapshot_w,
                         (unsigned)s_snapshot_h);
            }
        }

        if ((do_jpeg != FALSE) && ((do_snap != FALSE) || (s_snapshot_w > 0U))) {
            camera_sensor_jpeg_request();
            s_last_jpeg_tick = xTaskGetTickCount();
        }
    }
}

static status_t camera_sensor_preview_task_start(st7789_t *lcd)
{
    if (s_preview_task == NULL) {
        if (xTaskCreate(camera_sensor_preview_task,
                        "cam_sensor",
                        CAMERA_SENSOR_PREVIEW_TASK_STACK,
                        lcd,
                        CAMERA_SENSOR_PREVIEW_TASK_PRIO,
                        &s_preview_task) != pdPASS) {
            LOG_ERROR("camera sensor preview task create failed");
            return STATUS_FAIL;
        }
    }

    if (camera_sensor_jpeg_task_start() != STATUS_OK) {
        LOG_WARN("cam jpeg task start failed; sync encode fallback");
    }

    s_preview_lcd = lcd;
    return STATUS_OK;
}

status_t camera_sensor_init(void)
{
    ov2640_config_t cfg = {0};
    ov2640_pin_config_t pins;
    i2c_master_bus_handle_t i2c_bus = NULL;

    pins = camera_sensor_pin_config();
    if (!ov2640_pins_valid(&pins)) {
        LOG_WARN("OV2640 pins not configured in board.h");
        return STATUS_FAIL;
    }

    if (I2cGetMasterBusHandle((s32_t)BOARD_I2C_OV2640_SCCB_PORT, &i2c_bus) != TRUE) {
        LOG_ERROR("OV2640 I2C bus not ready (port %d)", (int)BOARD_I2C_OV2640_SCCB_PORT);
        return STATUS_FAIL;
    }

    cfg.sccb_write         = ov2640_sccb_write;
    cfg.sccb_read          = ov2640_sccb_read;
    cfg.sccb_write_read    = ov2640_sccb_write_read;
    cfg.sccb_addr7         = (uint8_t)BOARD_I2C_OV2640_SCCB_ADDR;
    cfg.pins               = pins;
    cfg.frame_width        = CAMERA_SENSOR_CAPTURE_WIDTH;
    cfg.frame_height       = CAMERA_SENSOR_CAPTURE_HEIGHT;
    cfg.format             = OV2640_FMT_RGB565;
    cfg.i2c_bus_handle     = i2c_bus;
    cfg.i2c_port           = (int8_t)BOARD_I2C_OV2640_SCCB_PORT;
    cfg.i2c_sda            = (int8_t)BOARD_I2C_BUS1_PIN_SDA;
    cfg.i2c_scl            = (int8_t)BOARD_I2C_BUS1_PIN_SCL;
    cfg.sensor_format_name = CAMERA_SENSOR_SENSOR_FORMAT;

    if (ov2640_init_with_config(&s_ov2640, &cfg) != OV2640_OK) {
        LOG_ERROR("ov2640_init_with_config failed");
        return STATUS_FAIL;
    }

    if (camera_sensor_snapshot_buf_ensure(CAMERA_SENSOR_SNAPSHOT_BYTES_MAX) != STATUS_OK) {
        LOG_ERROR("camera sensor snapshot buffer init failed");
        return STATUS_FAIL;
    }
    if (camera_sensor_jpeg_cache_ensure() != STATUS_OK) {
        LOG_WARN("camera jpeg cache alloc failed (web will encode on demand)");
    }

    {
        nvs_camera_settings_t st;

        if (nvs_camera_settings_get(&st)) {
            (void)camera_sensor_apply_settings(&st, FALSE);
            LOG_INFO("cam settings from NVS %ux%u q=%u gray=%u zoom=%u rot=%u",
                     (unsigned)st.web_width,
                     (unsigned)st.web_height,
                     (unsigned)st.quality,
                     (unsigned)st.grayscale,
                     (unsigned)st.zoom,
                     (unsigned)st.img_rotate);
        }
    }

    s_camera_ready = TRUE;
    LOG_INFO("OV2640 ready %ux%u", (unsigned)cfg.frame_width, (unsigned)cfg.frame_height);
    return STATUS_OK;
}

bool_t camera_sensor_is_ready(void)
{
    return (s_camera_ready == TRUE) && ov2640_is_initialized(&s_ov2640) ? TRUE : FALSE;
}

status_t camera_sensor_preview_start(void)
{
    st7789_t *lcd = BoardSt7789();
    TickType_t start;
    TickType_t budget;

    if (!camera_sensor_is_ready()) {
        return STATUS_FAIL;
    }

    if (camera_sensor_preview_task_start(lcd) != STATUS_OK) {
        return STATUS_FAIL;
    }

    if ((lcd != NULL) && st7789_is_initialized(lcd)) {
        if (camera_sensor_blit_strip_ensure(camera_sensor_strip_bytes(st7789_display_width(lcd))) != STATUS_OK) {
            LOG_WARN("camera sensor blit strip init failed (LCD preview disabled)");
        }
    }

    if (ov2640_set_frame_callback(&s_ov2640, camera_sensor_on_frame, NULL) != OV2640_OK) {
        LOG_WARN("ov2640_set_frame_callback failed");
    }

    s_frame_count    = 0U;
    s_lcd_blit_count = 0U;

    if (ov2640_start_stream(&s_ov2640) != OV2640_OK) {
        return STATUS_FAIL;
    }

    /* 等首帧：确认 DVP 有 EOF；超时不判死，预览任务仍会继续等帧。 */
    start  = xTaskGetTickCount();
    budget = pdMS_TO_TICKS(2000U);
    while ((xTaskGetTickCount() - start) < budget) {
        if (s_frame_count > 0U) {
            LOG_INFO("cam first frame ok frames=%u", (unsigned)s_frame_count);
            return STATUS_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    LOG_WARN("cam first frame timeout (no DVP EOF in 2s); keep waiting");
    return STATUS_OK;
}

uint32_t camera_sensor_get_frame_count(void)
{
    return s_frame_count;
}

uint32_t camera_sensor_get_lcd_blit_count(void)
{
    return s_lcd_blit_count;
}

status_t camera_sensor_preview_stop(void)
{
    if (!camera_sensor_is_ready()) {
        return STATUS_OK;
    }
    s_preview_lcd = NULL;
    return ov2640_stop_stream(&s_ov2640) == OV2640_OK ? STATUS_OK : STATUS_FAIL;
}

void camera_sensor_prepare_for_reboot(void)
{
    /* 等待进行中的 JPEG 编码结束，避免重启期间访问编码器。 */
    if (s_jpeg_mutex != NULL) {
        (void)xSemaphoreTake(s_jpeg_mutex, pdMS_TO_TICKS(500));
        (void)xSemaphoreGive(s_jpeg_mutex);
    }
}

uint16_t camera_sensor_get_snapshot_width(void)
{
    return s_snapshot_w;
}

uint16_t camera_sensor_get_snapshot_height(void)
{
    return s_snapshot_h;
}

status_t camera_sensor_copy_rgb565(uint8_t *out, uint32_t out_cap, uint16_t *out_w, uint16_t *out_h)
{
    uint32_t frame_bytes;
    uint16_t width;
    uint16_t height;
    uint8_t *pub;

    if ((out == NULL) || (out_w == NULL) || (out_h == NULL)) {
        return STATUS_INVALID_ARG;
    }
    if (camera_sensor_snapshot_mtx_init() != STATUS_OK) {
        return STATUS_FAIL;
    }
    if (xSemaphoreTake(s_snapshot_mtx, pdMS_TO_TICKS(50)) != pdTRUE) {
        return STATUS_TIMEOUT;
    }

    width = s_snapshot_w;
    height = s_snapshot_h;
    frame_bytes = (uint32_t)width * (uint32_t)height * 2U;
    pub = camera_sensor_pub_buf();
    if ((pub == NULL) || (width == 0U) || (height == 0U) || (frame_bytes == 0U) ||
        (frame_bytes > s_snapshot_bytes) || (out_cap < frame_bytes)) {
        (void)xSemaphoreGive(s_snapshot_mtx);
        return STATUS_FAIL;
    }

    (void)memcpy(out, pub, (size_t)frame_bytes);
    *out_w = width;
    *out_h = height;
    (void)xSemaphoreGive(s_snapshot_mtx);
    return STATUS_OK;
}

status_t camera_sensor_acquire_rgb565(const uint8_t **out, uint16_t *out_w, uint16_t *out_h, uint32_t *gen)
{
    uint8_t *pub;

    if ((out == NULL) || (out_w == NULL) || (out_h == NULL)) {
        return STATUS_INVALID_ARG;
    }
    if (camera_sensor_snapshot_mtx_init() != STATUS_OK) {
        return STATUS_FAIL;
    }
    if (xSemaphoreTake(s_snapshot_mtx, pdMS_TO_TICKS(50)) != pdTRUE) {
        return STATUS_TIMEOUT;
    }

    pub = camera_sensor_pub_buf();
    if ((pub == NULL) || (s_snapshot_w == 0U) || (s_snapshot_h == 0U)) {
        (void)xSemaphoreGive(s_snapshot_mtx);
        return STATUS_FAIL;
    }
    if (s_snap_detect_hold < CAMERA_SENSOR_SNAP_SLOT_COUNT) {
        (void)xSemaphoreGive(s_snapshot_mtx);
        return STATUS_FAIL;
    }

    s_snap_detect_hold = s_snap_pub;
    *out = pub;
    *out_w = s_snapshot_w;
    *out_h = s_snapshot_h;
    if (gen != NULL) {
        *gen = s_snap_gen;
    }
    (void)xSemaphoreGive(s_snapshot_mtx);
    return STATUS_OK;
}

void camera_sensor_release_rgb565(void)
{
    if (s_snapshot_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_snapshot_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_snap_detect_hold = CAMERA_SENSOR_DETECT_HOLD_NONE;
        (void)xSemaphoreGive(s_snapshot_mtx);
    }
}

uint16_t camera_sensor_get_web_width(void)
{
    return s_web_w;
}

uint16_t camera_sensor_get_web_height(void)
{
    return s_web_h;
}

uint8_t camera_sensor_get_jpeg_quality(void)
{
    return s_jpeg_quality;
}

uint8_t camera_sensor_get_grayscale(void)
{
    return s_grayscale;
}

uint8_t camera_sensor_get_zoom(void)
{
    return s_zoom;
}

uint8_t camera_sensor_get_img_rotate(void)
{
    return s_img_rotate;
}

uint8_t camera_sensor_get_flip_v(void)
{
    return s_flip_vertical;
}

uint8_t camera_sensor_get_flip_h(void)
{
    return s_flip_horizontal;
}

uint32_t camera_sensor_get_jpeg_seq(void)
{
    return s_jpeg_seq;
}

void camera_sensor_web_stream_enter(void)
{
    s_web_stream_clients++;
    if (s_web_stream_clients == 1U) {
        if (camera_ui_lcd_is_open() != FALSE) {
            s_lcd_held_for_stream = TRUE;
            if (camera_ui_lcd_close() != STATUS_OK) {
                s_lcd_held_for_stream = FALSE;
                LOG_WARN("cam exclusive: lcd_close failed");
            } else {
                LOG_INFO("cam exclusive: LCD off (MJPEG)");
            }
        }
    }
}

void camera_sensor_web_stream_leave(void)
{
    if (s_web_stream_clients > 0U) {
        s_web_stream_clients--;
    }
    if ((s_web_stream_clients == 0U) && (s_lcd_held_for_stream != FALSE)) {
        s_lcd_held_for_stream = FALSE;
        if (camera_ui_lcd_open() != STATUS_OK) {
            LOG_WARN("cam exclusive: lcd_open failed");
        } else {
            LOG_INFO("cam exclusive: LCD on (MJPEG end)");
        }
    }
}

bool_t camera_sensor_web_stream_active(void)
{
    return (s_web_stream_clients > 0U) ? TRUE : FALSE;
}

status_t camera_sensor_wait_jpeg_seq(uint32_t *inout_seq, uint32_t timeout_ms)
{
    uint32_t start;
    uint32_t last;

    if (inout_seq == NULL) {
        return STATUS_INVALID_ARG;
    }
    if (camera_sensor_jpeg_cache_ensure() != STATUS_OK) {
        return STATUS_FAIL;
    }

    last  = *inout_seq;
    start = (uint32_t)xTaskGetTickCount();
    for (;;) {
        uint32_t now_seq = s_jpeg_seq;
        uint32_t elapsed;
        uint32_t remain_ms;
        TickType_t wait_ticks;

        if (now_seq != last) {
            *inout_seq = now_seq;
            return STATUS_OK;
        }

        elapsed = (uint32_t)(xTaskGetTickCount() - start);
        if (elapsed >= pdMS_TO_TICKS(timeout_ms)) {
            return STATUS_TIMEOUT;
        }
        remain_ms  = timeout_ms - (elapsed * 1000U / (uint32_t)configTICK_RATE_HZ);
        wait_ticks = pdMS_TO_TICKS((remain_ms > 50U) ? 50U : remain_ms);
        if (wait_ticks == 0U) {
            wait_ticks = 1U;
        }
        (void)xSemaphoreTake(s_jpeg_seq_sem, wait_ticks);
    }
}

uint32_t camera_sensor_get_web_poll_ms(void)
{
    uint16_t fw = s_web_w;
    uint16_t fh = s_web_h;

    if ((s_img_rotate == 1U) || (s_img_rotate == 3U)) {
        fw = s_web_h;
        fh = s_web_w;
    }
    if ((fw >= 640U) || (fh >= 480U)) {
        return 180U;
    }
    if ((fw >= 320U) || (fh >= 240U)) {
        return (uint32_t)CAMERA_SENSOR_WEB_FRAME_MS;
    }
    return 100U;
}

status_t camera_sensor_apply_settings(const nvs_camera_settings_t *cfg, bool_t persist)
{
    nvs_camera_settings_t st;

    if (cfg == NULL) {
        return STATUS_INVALID_ARG;
    }
    st = *cfg;
    st.magic = NVS_CAMERA_SETTINGS_MAGIC;
    if (!nvs_camera_settings_validate(&st)) {
        return STATUS_INVALID_ARG;
    }

    /* 采集为 240×240：更大预览尺寸只会放大，钳到原生分辨率。 */
    if ((st.web_width > CAMERA_SENSOR_CAPTURE_WIDTH) ||
        (st.web_height > CAMERA_SENSOR_CAPTURE_HEIGHT)) {
        st.web_width  = CAMERA_SENSOR_CAPTURE_WIDTH;
        st.web_height = CAMERA_SENSOR_CAPTURE_HEIGHT;
    }

    s_web_w            = st.web_width;
    s_web_h            = st.web_height;
    s_jpeg_quality     = st.quality;
    s_grayscale        = st.grayscale;
    s_zoom             = st.zoom;
    s_img_rotate       = st.img_rotate;
    s_flip_vertical    = st.flip_v;
    s_flip_horizontal  = st.flip_h;

    if (s_jpeg_cache_mtx != NULL) {
        if (xSemaphoreTake(s_jpeg_cache_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
            s_jpeg_cache_len = 0U;
            (void)xSemaphoreGive(s_jpeg_cache_mtx);
        }
    }
    s_last_snapshot_tick = 0U;

    if (persist != FALSE) {
        nvs_camera_settings_t verify;

        if (!nvs_camera_settings_set(&st)) {
            LOG_WARN("cam settings NVS save failed");
            return STATUS_FAIL;
        }
        /* 写后回读，避免 commit 看似成功但键值未落盘。 */
        if (!nvs_camera_settings_get(&verify) ||
            (verify.web_width != st.web_width) || (verify.web_height != st.web_height) ||
            (verify.quality != st.quality) || (verify.grayscale != st.grayscale) ||
            (verify.zoom != st.zoom) || (verify.img_rotate != st.img_rotate) ||
            (verify.flip_v != st.flip_v) || (verify.flip_h != st.flip_h)) {
            LOG_WARN("cam settings NVS verify mismatch");
            return STATUS_FAIL;
        }
        LOG_INFO("cam cfg saved NVS %ux%u q=%u gray=%u zoom=%u rot=%u flip=%u/%u",
                 (unsigned)s_web_w,
                 (unsigned)s_web_h,
                 (unsigned)s_jpeg_quality,
                 (unsigned)s_grayscale,
                 (unsigned)s_zoom,
                 (unsigned)s_img_rotate,
                 (unsigned)s_flip_vertical,
                 (unsigned)s_flip_horizontal);
    } else {
        LOG_INFO("cam cfg preview %ux%u q=%u gray=%u zoom=%u rot=%u flip=%u/%u",
                 (unsigned)s_web_w,
                 (unsigned)s_web_h,
                 (unsigned)s_jpeg_quality,
                 (unsigned)s_grayscale,
                 (unsigned)s_zoom,
                 (unsigned)s_img_rotate,
                 (unsigned)s_flip_vertical,
                 (unsigned)s_flip_horizontal);
    }
    return STATUS_OK;
}

status_t camera_sensor_set_web_cfg(uint16_t width, uint16_t height, uint8_t quality)
{
    nvs_camera_settings_t st;

    nvs_camera_settings_default(&st);
    st.web_width  = width;
    st.web_height = height;
    st.quality    = quality;
    st.grayscale  = s_grayscale;
    st.zoom       = s_zoom;
    st.img_rotate = s_img_rotate;
    st.flip_v     = s_flip_vertical;
    st.flip_h     = s_flip_horizontal;
    return camera_sensor_apply_settings(&st, TRUE);
}

status_t camera_sensor_copy_jpeg_cache(uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    if ((out == NULL) || (out_cap == 0U) || (out_len == NULL)) {
        return STATUS_INVALID_ARG;
    }
    *out_len = 0U;
    if ((s_jpeg_cache == NULL) || (s_jpeg_cache_mtx == NULL)) {
        return STATUS_FAIL;
    }
    if (xSemaphoreTake(s_jpeg_cache_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        return STATUS_FAIL;
    }
    if ((s_jpeg_cache_len == 0U) || (s_jpeg_cache_len > out_cap)) {
        (void)xSemaphoreGive(s_jpeg_cache_mtx);
        return STATUS_FAIL;
    }
    (void)memcpy(out, s_jpeg_cache, (size_t)s_jpeg_cache_len);
    *out_len = s_jpeg_cache_len;
    (void)xSemaphoreGive(s_jpeg_cache_mtx);
    return STATUS_OK;
}

status_t camera_sensor_snapshot_jpeg(uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    uint32_t     frame_bytes;
    uint16_t     width;
    uint16_t     height;
    int          jpeg_size = 0;
    jpeg_error_t jerr;

    if ((out == NULL) || (out_cap == 0U) || (out_len == NULL) || !camera_sensor_is_ready()) {
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return STATUS_INVALID_ARG;
    }

    *out_len = 0U;

    /* 预览优先走 cache，避免 HTTP 路径重复编码。 */
    if (camera_sensor_copy_jpeg_cache(out, out_cap, out_len) == STATUS_OK) {
        return STATUS_OK;
    }

    if (camera_sensor_snapshot_mtx_init() != STATUS_OK) {
        return STATUS_FAIL;
    }
    if (xSemaphoreTake(s_snapshot_mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        return STATUS_FAIL;
    }

    width       = s_snapshot_w;
    height      = s_snapshot_h;
    frame_bytes = (uint32_t)width * (uint32_t)height * 2U;
    {
        uint8_t *pub = camera_sensor_pub_buf();

        if ((pub == NULL) || (s_snapshot_copy == NULL) || (frame_bytes == 0U) ||
            (frame_bytes > s_snapshot_bytes)) {
            (void)xSemaphoreGive(s_snapshot_mtx);
            return STATUS_FAIL;
        }
        (void)memcpy(s_snapshot_copy, pub, (size_t)frame_bytes);
    }
    (void)xSemaphoreGive(s_snapshot_mtx);

#if CAMERA_DETECT_OVERLAY_WEB
    camera_sensor_overlay_detect_on_rgb565(s_snapshot_copy, width, height);
#endif

    if (camera_sensor_jpeg_enc_open() != STATUS_OK) {
        return STATUS_FAIL;
    }

    if (xSemaphoreTake(s_jpeg_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return STATUS_FAIL;
    }

    s_jpeg_busy = 1U;
    jerr = jpeg_enc_process(s_jpeg_enc,
                            s_snapshot_copy,
                            (int)frame_bytes,
                            out,
                            (int)out_cap,
                            &jpeg_size);
    s_jpeg_busy = 0U;
    (void)xSemaphoreGive(s_jpeg_mutex);

    if ((jerr != JPEG_ERR_OK) || (jpeg_size <= 0)) {
        LOG_WARN("camera jpeg_enc_process failed %d", (int)jerr);
        return STATUS_FAIL;
    }

    *out_len = (uint32_t)jpeg_size;
    return STATUS_OK;
}
