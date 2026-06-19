/**
 * @file lcd_video.c
 * @brief MJPEG-in-AVI 子集解析、esp_new_jpeg 解码、RGB565 条带推屏。
 */

#include "lcd_video.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "board.h"
#include "button.h"
#include "dma.h"
#include "log.h"
#include "sdcard.h"
#include "st7789.h"

#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LCD_VIDEO_PANEL_W             (240U)
#define LCD_VIDEO_PANEL_H             (135U)
#define LCD_VIDEO_RGB565_FULL_BYTES   (64800U)
#define LCD_VIDEO_JPEG_MAX_BYTES      (65536U)
#define LCD_VIDEO_MAX_FILES           (16U)
#define LCD_VIDEO_PATH_MAX            (320U)
#define LCD_VIDEO_STRIP_BYTES_MAX     (8192U)
#define LCD_VIDEO_BENCH_DEFAULT_FRAMES (100U)

static char s_paths[LCD_VIDEO_MAX_FILES][LCD_VIDEO_PATH_MAX];
static uint8_t s_count;

static volatile bool s_stop_requested;
static volatile bool s_playing;

static SemaphoreHandle_t s_pending_mutex;
static lcd_video_pending_kind_t s_pending_kind;
static char s_pending_path[LCD_VIDEO_PATH_MAX];
static uint8_t s_pending_index;
static uint32_t s_pending_bench_frames;

static const char *video_dir_path(void)
{
    static char s_dir[64];

    (void)snprintf(s_dir, sizeof(s_dir), "%s%s", BOARD_SDCARD_MOUNT_POINT, LCD_VIDEO_DIR_SUFFIX);
    return s_dir;
}

static bool path_suffix_icase(const char *path, const char *suf)
{
    size_t lp;
    size_t ls;

    if ((path == NULL) || (suf == NULL)) {
        return false;
    }
    lp = strlen(path);
    ls = strlen(suf);
    if (lp < ls) {
        return false;
    }
    return strcasecmp(path + (lp - ls), suf) == 0;
}

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(n, MALLOC_CAP_DEFAULT);
    }
    return p;
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool fourcc_eq(const char id[4], const char *s)
{
    return (memcmp(id, s, 4U) == 0);
}

static bool chunk_is_video_frame(const char id[4])
{
    return ((id[0] == '0') && (id[1] == '0') &&
            ((id[2] == 'd') || (id[2] == 'D')) &&
            ((id[3] == 'c') || (id[3] == 'C') || (id[3] == 'b') || (id[3] == 'B')));
}

static void paths_sort(void)
{
    uint8_t i;
    uint8_t j;

    for (i = 0U; i < s_count; i++) {
        for (j = (uint8_t)(i + 1U); j < s_count; j++) {
            if (strcmp(s_paths[i], s_paths[j]) > 0) {
                char tmp[LCD_VIDEO_PATH_MAX];
                (void)memcpy(tmp, s_paths[i], sizeof(tmp));
                (void)memcpy(s_paths[i], s_paths[j], sizeof(s_paths[i]));
                (void)memcpy(s_paths[j], tmp, sizeof(s_paths[j]));
            }
        }
    }
}

status_t lcd_video_scan(void)
{
    DIR           *dir;
    struct dirent *ent;
    const char    *vdir = video_dir_path();

    s_count = 0U;
    if (sdcard_get_card() == NULL) {
        return STATUS_INVALID_STATE;
    }

    dir = opendir(vdir);
    if (dir == NULL) {
        LOG_WARN("lcd_video: opendir %s failed errno %d", vdir, (int)errno);
        return STATUS_FAIL;
    }

    while ((ent = readdir(dir)) != NULL) {
        struct stat st;

        if (ent->d_name[0] == 0) {
            continue;
        }
        if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
            continue;
        }
        if (!path_suffix_icase(ent->d_name, ".avi")) {
            continue;
        }
        if (s_count >= LCD_VIDEO_MAX_FILES) {
            break;
        }
        (void)snprintf(s_paths[s_count], sizeof(s_paths[s_count]), "%s/%s", vdir, ent->d_name);
        if (stat(s_paths[s_count], &st) != 0) {
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        s_count++;
    }
    (void)closedir(dir);
    paths_sort();

    if (s_count > 0U) {
        LOG_INFO("lcd_video: %u files under %s", (unsigned int)s_count, vdir);
    } else {
        LOG_WARN("lcd_video: no .avi under %s", vdir);
    }
    return STATUS_OK;
}

uint8_t lcd_video_count(void)
{
    return s_count;
}

status_t lcd_video_path_at(uint8_t idx, char *out, size_t out_cap)
{
    if ((out == NULL) || (out_cap == 0U)) {
        return STATUS_INVALID_ARG;
    }
    if (s_count == 0U) {
        out[0] = '\0';
        return STATUS_FAIL;
    }
    (void)snprintf(out, out_cap, "%s", s_paths[idx % s_count]);
    return STATUS_OK;
}

static status_t resolve_play_path(const char *path_in, char *path_out, size_t out_cap)
{
    struct stat st;

    if ((path_in == NULL) || (path_out == NULL) || (out_cap == 0U)) {
        return STATUS_INVALID_ARG;
    }
    if (path_in[0] == '/') {
        (void)snprintf(path_out, out_cap, "%s", path_in);
    } else {
        (void)snprintf(path_out, out_cap, "%s/%s", video_dir_path(), path_in);
    }
    if (stat(path_out, &st) != 0) {
        (void)snprintf(path_out, out_cap, "%s/%s", BOARD_SDCARD_MOUNT_POINT, path_in);
    }
    if (stat(path_out, &st) != 0) {
        LOG_WARN("lcd_video: not found %s", path_in);
        return STATUS_FAIL;
    }
    if (!S_ISREG(st.st_mode)) {
        return STATUS_INVALID_ARG;
    }
    return STATUS_OK;
}

void lcd_video_request_stop(void)
{
    s_stop_requested = true;
}

bool lcd_video_is_playing(void)
{
    return s_playing;
}

static void pending_init_once(void)
{
    if (s_pending_mutex == NULL) {
        s_pending_mutex = xSemaphoreCreateMutex();
    }
}

bool lcd_video_post_play_path(const char *path)
{
    char resolved[LCD_VIDEO_PATH_MAX];

    pending_init_once();
    if ((path == NULL) || (path[0] == '\0')) {
        return false;
    }
    if (resolve_play_path(path, resolved, sizeof(resolved)) != STATUS_OK) {
        return false;
    }
    if (s_pending_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_pending_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    if (s_playing || (s_pending_kind != LCD_VIDEO_PENDING_NONE)) {
        xSemaphoreGive(s_pending_mutex);
        return false;
    }
    s_pending_kind = LCD_VIDEO_PENDING_PLAY_PATH;
    (void)snprintf(s_pending_path, sizeof(s_pending_path), "%s", resolved);
    xSemaphoreGive(s_pending_mutex);
    return true;
}

bool lcd_video_post_play_index(uint8_t idx)
{
    pending_init_once();
    if (s_count == 0U) {
        return false;
    }
    if (s_pending_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_pending_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    if (s_playing || (s_pending_kind != LCD_VIDEO_PENDING_NONE)) {
        xSemaphoreGive(s_pending_mutex);
        return false;
    }
    s_pending_kind   = LCD_VIDEO_PENDING_PLAY_INDEX;
    s_pending_index  = (uint8_t)(idx % s_count);
    s_pending_path[0] = '\0';
    xSemaphoreGive(s_pending_mutex);
    return true;
}

bool lcd_video_post_benchmark(const char *path, uint32_t max_frames)
{
    char resolved[LCD_VIDEO_PATH_MAX];

    pending_init_once();
    if ((path == NULL) || (path[0] == '\0')) {
        return false;
    }
    if (resolve_play_path(path, resolved, sizeof(resolved)) != STATUS_OK) {
        return false;
    }
    if (s_pending_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_pending_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    if (s_playing || (s_pending_kind != LCD_VIDEO_PENDING_NONE)) {
        xSemaphoreGive(s_pending_mutex);
        return false;
    }
    s_pending_kind         = LCD_VIDEO_PENDING_BENCH;
    s_pending_bench_frames = (max_frames == 0U) ? LCD_VIDEO_BENCH_DEFAULT_FRAMES : max_frames;
    (void)snprintf(s_pending_path, sizeof(s_pending_path), "%s", resolved);
    xSemaphoreGive(s_pending_mutex);
    return true;
}

bool lcd_video_take_pending_play(lcd_video_pending_kind_t *kind, char *path_out, size_t path_cap, uint8_t *index_out,
                                 uint32_t *bench_frames_out)
{
    pending_init_once();
    if (kind == NULL) {
        return false;
    }
    if (s_pending_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_pending_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }
    *kind = s_pending_kind;
    if (*kind == LCD_VIDEO_PENDING_NONE) {
        xSemaphoreGive(s_pending_mutex);
        return false;
    }
    if ((path_out != NULL) && (path_cap > 0U)) {
        (void)snprintf(path_out, path_cap, "%s", s_pending_path);
    }
    if ((*kind == LCD_VIDEO_PENDING_PLAY_INDEX) && (index_out != NULL)) {
        *index_out = s_pending_index;
    }
    if ((*kind == LCD_VIDEO_PENDING_BENCH) && (bench_frames_out != NULL)) {
        *bench_frames_out = s_pending_bench_frames;
    }
    s_pending_kind = LCD_VIDEO_PENDING_NONE;
    s_pending_path[0] = '\0';
    s_pending_bench_frames = 0U;
    xSemaphoreGive(s_pending_mutex);
    return true;
}

typedef struct {
    FILE    *fp;
    long     movi_data_start;
    long     movi_data_end;
    long     pos;
    uint32_t us_per_frame;
} avi_reader_t;

static status_t avi_read_exact(FILE *fp, void *buf, size_t n)
{
    size_t got = fread(buf, 1U, n, fp);
    if (got != n) {
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

static status_t avi_skip(FILE *fp, long *pos, uint32_t n)
{
    if (fseek(fp, (long)n, SEEK_CUR) != 0) {
        return STATUS_FAIL;
    }
    *pos += (long)n;
    return STATUS_OK;
}

static status_t avi_parse_hdrl_chunk(FILE *fp, long chunk_end, uint32_t *us_per_frame_out)
{
    char     id[4];
    uint8_t  hdr[56];
    long     pos = ftell(fp);

    while (pos < chunk_end) {
        uint32_t size;
        uint8_t  szbuf[4];

        if (avi_read_exact(fp, id, 4U) != STATUS_OK) {
            return STATUS_FAIL;
        }
        if (avi_read_exact(fp, szbuf, 4U) != STATUS_OK) {
            return STATUS_FAIL;
        }
        size = read_u32_le(szbuf);
        pos += 8L;

        if (fourcc_eq(id, "avih")) {
            if (size < 32U) {
                return STATUS_INVALID_ARG;
            }
            if (avi_read_exact(fp, hdr, 32U) != STATUS_OK) {
                return STATUS_FAIL;
            }
            *us_per_frame_out = read_u32_le(hdr + 0U);
            if (size > 32U) {
                (void)avi_skip(fp, &pos, (uint32_t)(size - 32U));
            }
        } else if (fourcc_eq(id, "LIST")) {
            char list_type[4];
            long list_end;

            if (size < 4U) {
                return STATUS_INVALID_ARG;
            }
            if (avi_read_exact(fp, list_type, 4U) != STATUS_OK) {
                return STATUS_FAIL;
            }
            pos += 4L;
            list_end = pos + (long)size - 4L;
            if (fourcc_eq(list_type, "strl")) {
                while (pos < list_end) {
                    char     sid[4];
                    uint32_t ssz;

                    if (avi_read_exact(fp, sid, 4U) != STATUS_OK) {
                        break;
                    }
                    if (avi_read_exact(fp, szbuf, 4U) != STATUS_OK) {
                        break;
                    }
                    ssz = read_u32_le(szbuf);
                    pos += 8L;
                    if (avi_skip(fp, &pos, ssz + (ssz & 1U)) != STATUS_OK) {
                        break;
                    }
                }
            } else {
                if (avi_skip(fp, &pos, size - 4U + (size & 1U)) != STATUS_OK) {
                    return STATUS_FAIL;
                }
            }
            pos = list_end;
            (void)fseek(fp, pos, SEEK_SET);
        } else {
            if (avi_skip(fp, &pos, size + (size & 1U)) != STATUS_OK) {
                return STATUS_FAIL;
            }
        }
        pos = ftell(fp);
    }
    return STATUS_OK;
}

static status_t avi_open(const char *path, avi_reader_t *ar)
{
    uint8_t riff[12];
    long    file_end;
    long    pos;

    (void)memset(ar, 0, sizeof(*ar));
    ar->fp = fopen(path, "rb");
    if (ar->fp == NULL) {
        LOG_WARN("lcd_video: fopen %s errno %d", path, (int)errno);
        return STATUS_FAIL;
    }
    if (fseek(ar->fp, 0L, SEEK_END) != 0) {
        (void)fclose(ar->fp);
        ar->fp = NULL;
        return STATUS_FAIL;
    }
    file_end = ftell(ar->fp);
    (void)fseek(ar->fp, 0L, SEEK_SET);

    if (avi_read_exact(ar->fp, riff, 12U) != STATUS_OK) {
        (void)fclose(ar->fp);
        ar->fp = NULL;
        return STATUS_FAIL;
    }
    if (!fourcc_eq((const char *)riff, "RIFF") || !fourcc_eq((const char *)(riff + 8), "AVI ")) {
        LOG_WARN("lcd_video: not RIFF/AVI %s", path);
        (void)fclose(ar->fp);
        ar->fp = NULL;
        return STATUS_INVALID_ARG;
    }

    ar->us_per_frame = 83333U; /* 12 FPS default */
    pos              = 12L;

    while (pos < file_end) {
        char     id[4];
        uint8_t  szbuf[4];
        uint32_t size;

        if (avi_read_exact(ar->fp, id, 4U) != STATUS_OK) {
            break;
        }
        if (avi_read_exact(ar->fp, szbuf, 4U) != STATUS_OK) {
            break;
        }
        size = read_u32_le(szbuf);
        pos += 8L;

        if (fourcc_eq(id, "LIST")) {
            char list_type[4];
            long list_content_start;
            long list_end;

            if (size < 4U) {
                break;
            }
            if (avi_read_exact(ar->fp, list_type, 4U) != STATUS_OK) {
                break;
            }
            pos += 4L;
            list_content_start = ftell(ar->fp);
            list_end           = list_content_start + (long)size - 4L;

            if (fourcc_eq(list_type, "hdrl")) {
                (void)avi_parse_hdrl_chunk(ar->fp, list_end, &ar->us_per_frame);
            } else if (fourcc_eq(list_type, "movi")) {
                ar->movi_data_start = list_content_start;
                ar->movi_data_end   = list_end;
            }
            (void)fseek(ar->fp, list_end, SEEK_SET);
            pos = list_end;
        } else {
            if (avi_skip(ar->fp, &pos, size + (size & 1U)) != STATUS_OK) {
                break;
            }
        }
    }

    if ((ar->movi_data_end <= ar->movi_data_start) || (ar->movi_data_start <= 0L)) {
        LOG_WARN("lcd_video: movi not found in %s", path);
        (void)fclose(ar->fp);
        ar->fp = NULL;
        return STATUS_INVALID_ARG;
    }

    ar->pos = ar->movi_data_start;
    (void)fseek(ar->fp, ar->pos, SEEK_SET);
    return STATUS_OK;
}

static void avi_rewind_movi(avi_reader_t *ar)
{
    ar->pos = ar->movi_data_start;
    (void)fseek(ar->fp, ar->pos, SEEK_SET);
}

static void avi_close(avi_reader_t *ar)
{
    if (ar->fp != NULL) {
        (void)fclose(ar->fp);
        ar->fp = NULL;
    }
}

static status_t avi_next_frame_in_range(FILE *fp, long *pos, long end, uint8_t *buf, size_t buf_cap, size_t *out_len)
{
    while (*pos < end) {
        char     id[4];
        uint8_t  szbuf[4];
        uint32_t size;

        if (avi_read_exact(fp, id, 4U) != STATUS_OK) {
            return STATUS_FAIL;
        }
        if (avi_read_exact(fp, szbuf, 4U) != STATUS_OK) {
            return STATUS_FAIL;
        }
        size = read_u32_le(szbuf);
        *pos += 8L;

        if (fourcc_eq(id, "LIST")) {
            char list_type[4];
            long list_end;

            if (size < 4U) {
                return STATUS_INVALID_ARG;
            }
            if (avi_read_exact(fp, list_type, 4U) != STATUS_OK) {
                return STATUS_FAIL;
            }
            *pos += 4L;
            list_end = *pos + (long)size - 4L;
            {
                status_t st = avi_next_frame_in_range(fp, pos, list_end, buf, buf_cap, out_len);
                if (st == STATUS_OK) {
                    return st;
                }
            }
            *pos = list_end;
            (void)fseek(fp, *pos, SEEK_SET);
        } else if (chunk_is_video_frame(id)) {
            if (size == 0U) {
                continue;
            }
            if (size > buf_cap) {
                LOG_WARN("lcd_video: frame %u > buf %u", (unsigned int)size, (unsigned int)buf_cap);
                if (avi_skip(fp, pos, size + (size & 1U)) != STATUS_OK) {
                    return STATUS_FAIL;
                }
                return STATUS_INVALID_ARG;
            }
            if (avi_read_exact(fp, buf, (size_t)size) != STATUS_OK) {
                return STATUS_FAIL;
            }
            *pos += (long)size;
            if ((size & 1U) != 0U) {
                (void)fseek(fp, 1L, SEEK_CUR);
                *pos += 1L;
            }
            *out_len = (size_t)size;
            return STATUS_OK;
        } else {
            if (avi_skip(fp, pos, size + (size & 1U)) != STATUS_OK) {
                return STATUS_FAIL;
            }
        }
    }
    return STATUS_FAIL;
}

static status_t avi_read_next_jpeg(avi_reader_t *ar, uint8_t *buf, size_t buf_cap, size_t *out_len)
{
    status_t st = avi_next_frame_in_range(ar->fp, &ar->pos, ar->movi_data_end, buf, buf_cap, out_len);
    if (st == STATUS_OK) {
        return st;
    }
    return STATUS_FAIL;
}

static uint32_t rgb565_strip_bytes(uint16_t dst_w)
{
    uint32_t row_b    = (uint32_t)dst_w * 2U;
    uint32_t strip_max = (uint32_t)BOARD_ST7789_SPI_MAX_TX;

    if (strip_max > LCD_VIDEO_STRIP_BYTES_MAX) {
        strip_max = LCD_VIDEO_STRIP_BYTES_MAX;
    }
    strip_max = (strip_max / row_b) * row_b;
    if (strip_max == 0U) {
        strip_max = row_b;
    }
    return strip_max;
}

static status_t blit_rgb565_full(st7789_t *lcd, uint16_t dst_w, uint16_t dst_h, const uint8_t *rgb565)
{
    const uint32_t need = (uint32_t)dst_w * (uint32_t)dst_h * 2U;
    uint32_t       strip_max;
    void          *buf;
    uint32_t       remain;
    const uint8_t *src = rgb565;

    strip_max = rgb565_strip_bytes(dst_w);
    buf       = DmaMalloc((usize_t)strip_max);
    if (buf == NULL_PTR) {
        return STATUS_NO_MEM;
    }

    if (st7789_set_window(lcd, 0U, 0U, (uint16_t)(dst_w - 1U), (uint16_t)(dst_h - 1U)) != ST7789_OK) {
        DmaFree(buf);
        return STATUS_FAIL;
    }

    remain = need;
    while (remain > 0U) {
        size_t chunk = (remain > strip_max) ? (size_t)strip_max : (size_t)remain;

        (void)memcpy(buf, src, chunk);
        src += chunk;
        if (st7789_write_pixel_bytes(lcd, (const uint8_t *)buf, (uint32_t)chunk) != ST7789_OK) {
            st7789_end_write(lcd);
            DmaFree(buf);
            return STATUS_FAIL;
        }
        remain -= (uint32_t)chunk;
    }
    st7789_end_write(lcd);
    DmaFree(buf);
    return STATUS_OK;
}

static void poll_stop_from_button(void)
{
    btn_id_e    bid;
    btn_event_e bev;

    button_last_event_get(&bid, &bev);
    if ((bev == BTN_EVENT_SINGLE_CLICK) && (bid == BTN_ID_GPIO0)) {
        button_last_event_clear();
        s_stop_requested = true;
    }
}

typedef struct {
    jpeg_dec_handle_t dec;
    uint8_t          *jpeg_buf;
    size_t            jpeg_cap;
    uint8_t          *rgb_buf;
    size_t            rgb_cap;
} jpeg_ctx_t;

static status_t jpeg_ctx_init(jpeg_ctx_t *ctx)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();

    (void)memset(ctx, 0, sizeof(*ctx));
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;

    ctx->jpeg_cap = LCD_VIDEO_JPEG_MAX_BYTES;
    ctx->jpeg_buf = (uint8_t *)psram_alloc(ctx->jpeg_cap);
    if (ctx->jpeg_buf == NULL) {
        return STATUS_NO_MEM;
    }

    if (jpeg_dec_open(&cfg, &ctx->dec) != JPEG_ERR_OK) {
        free(ctx->jpeg_buf);
        ctx->jpeg_buf = NULL;
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

static void jpeg_ctx_free(jpeg_ctx_t *ctx)
{
    if (ctx->dec != NULL) {
        (void)jpeg_dec_close(ctx->dec);
        ctx->dec = NULL;
    }
    if (ctx->rgb_buf != NULL) {
        jpeg_free_align(ctx->rgb_buf);
        ctx->rgb_buf = NULL;
    }
    if (ctx->jpeg_buf != NULL) {
        free(ctx->jpeg_buf);
        ctx->jpeg_buf = NULL;
    }
}

static status_t jpeg_decode_frame(jpeg_ctx_t *ctx, const uint8_t *jpeg, size_t jpeg_len, uint16_t expect_w,
                                  uint16_t expect_h, uint32_t *decode_us_out)
{
    jpeg_dec_io_t         io;
    jpeg_dec_header_info_t hdr;
    jpeg_error_t          je;
    int                   out_len = 0;
    int64_t               t0;
    int64_t               t1;

    (void)memset(&io, 0, sizeof(io));
    (void)memset(&hdr, 0, sizeof(hdr));
    io.inbuf     = (uint8_t *)jpeg;
    io.inbuf_len = (int)jpeg_len;

    je = jpeg_dec_parse_header(ctx->dec, &io, &hdr);
    if (je != JPEG_ERR_OK) {
        LOG_WARN("lcd_video: jpeg header err %d", (int)je);
        return STATUS_INVALID_ARG;
    }
    if ((hdr.width != expect_w) || (hdr.height != expect_h)) {
        LOG_WARN("lcd_video: jpeg %ux%u != panel %ux%u", (unsigned int)hdr.width, (unsigned int)hdr.height,
                 (unsigned int)expect_w, (unsigned int)expect_h);
        return STATUS_INVALID_ARG;
    }

    je = jpeg_dec_get_outbuf_len(ctx->dec, &out_len);
    if ((je != JPEG_ERR_OK) || (out_len <= 0)) {
        return STATUS_FAIL;
    }
    if (((size_t)out_len != LCD_VIDEO_RGB565_FULL_BYTES) ||
        ((size_t)out_len > ctx->rgb_cap)) {
        if (ctx->rgb_buf != NULL) {
            jpeg_free_align(ctx->rgb_buf);
            ctx->rgb_buf = NULL;
        }
        ctx->rgb_cap = (size_t)out_len;
        ctx->rgb_buf = (uint8_t *)jpeg_calloc_align(ctx->rgb_cap, 16);
        if (ctx->rgb_buf == NULL) {
            return STATUS_NO_MEM;
        }
    }

    io.outbuf = ctx->rgb_buf;
    t0        = esp_timer_get_time();
    je        = jpeg_dec_process(ctx->dec, &io);
    t1        = esp_timer_get_time();
    if (decode_us_out != NULL) {
        *decode_us_out = (uint32_t)(t1 - t0);
    }
    if (je != JPEG_ERR_OK) {
        LOG_WARN("lcd_video: jpeg decode err %d", (int)je);
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

static status_t lcd_video_play_internal(st7789_t *lcd, const char *path, uint32_t max_frames, bool bench_mode)
{
    avi_reader_t ar;
    jpeg_ctx_t   jctx;
    status_t     st;
    uint16_t     dst_w;
    uint16_t     dst_h;
    uint32_t     frame_idx = 0U;
    uint64_t     total_us  = 0U;
    uint64_t     read_us   = 0U;
    uint64_t     dec_us    = 0U;
    uint64_t     blit_us   = 0U;

    if ((lcd == NULL) || !st7789_is_initialized(lcd) || (path == NULL)) {
        return STATUS_INVALID_ARG;
    }
    if (sdcard_get_card() == NULL) {
        return STATUS_INVALID_STATE;
    }

    dst_w = st7789_display_width(lcd);
    dst_h = st7789_display_height(lcd);
    if (((uint32_t)dst_w * (uint32_t)dst_h * 2U) != LCD_VIDEO_RGB565_FULL_BYTES) {
        LOG_ERROR("lcd_video: panel not 240x135");
        return STATUS_INVALID_ARG;
    }

    st = avi_open(path, &ar);
    if (st != STATUS_OK) {
        return st;
    }
    st = jpeg_ctx_init(&jctx);
    if (st != STATUS_OK) {
        avi_close(&ar);
        return st;
    }

    s_stop_requested = false;
    s_playing        = true;
    LOG_INFO("lcd_video: play %s us_per_frame=%u", path, (unsigned int)ar.us_per_frame);

    for (;;) {
        size_t   jpeg_len = 0U;
        int64_t  t0;
        int64_t  t1;
        uint32_t du = 0U;
        int64_t  frame_start = esp_timer_get_time();

        if (s_stop_requested) {
            st = STATUS_OK;
            break;
        }
        poll_stop_from_button();
        if (s_stop_requested) {
            st = STATUS_OK;
            break;
        }

        if ((max_frames > 0U) && (frame_idx >= max_frames)) {
            st = STATUS_OK;
            break;
        }

        t0 = esp_timer_get_time();
        st = avi_read_next_jpeg(&ar, jctx.jpeg_buf, jctx.jpeg_cap, &jpeg_len);
        t1 = esp_timer_get_time();
        if (st != STATUS_OK) {
            avi_rewind_movi(&ar);
            if (bench_mode) {
                break;
            }
            continue;
        }
        read_us += (uint64_t)(t1 - t0);

        st = jpeg_decode_frame(&jctx, jctx.jpeg_buf, jpeg_len, dst_w, dst_h, &du);
        if (st != STATUS_OK) {
            continue;
        }
        dec_us += du;

        t0 = esp_timer_get_time();
        st = blit_rgb565_full(lcd, dst_w, dst_h, jctx.rgb_buf);
        t1 = esp_timer_get_time();
        if (st != STATUS_OK) {
            break;
        }
        blit_us += (uint64_t)(t1 - t0);

        frame_idx++;
        total_us += (uint64_t)(t1 - frame_start);

        if (!bench_mode) {
            int64_t elapsed = esp_timer_get_time() - frame_start;
            int64_t target  = (int64_t)ar.us_per_frame;

            if ((target > 0) && (elapsed < target)) {
                vTaskDelay(pdMS_TO_TICKS((uint32_t)((target - elapsed + 999LL) / 1000LL)));
            } else {
                vTaskDelay(1);
            }
        } else {
            vTaskDelay(1);
        }
    }

    if ((bench_mode) && (frame_idx > 0U)) {
        float avg_fps = (float)frame_idx * 1000000.0f / (float)total_us;
        LOG_INFO("lcd_video bench: frames=%u avg_fps=%.2f read_ms=%.2f dec_ms=%.2f blit_ms=%.2f",
                 (unsigned int)frame_idx, (double)avg_fps, (double)((float)read_us / (1000.0f * (float)frame_idx)),
                 (double)((float)dec_us / (1000.0f * (float)frame_idx)),
                 (double)((float)blit_us / (1000.0f * (float)frame_idx)));
    }

    jpeg_ctx_free(&jctx);
    avi_close(&ar);
    s_playing = false;
    return st;
}

status_t lcd_video_read_first_jpeg(const char *path, uint8_t *buf, size_t buf_cap, size_t *out_len)
{
    char         resolved[LCD_VIDEO_PATH_MAX];
    avi_reader_t ar;
    status_t     st;

    if ((path == NULL) || (buf == NULL) || (out_len == NULL) || (buf_cap == 0U)) {
        return STATUS_INVALID_ARG;
    }
    if (sdcard_get_card() == NULL) {
        return STATUS_INVALID_STATE;
    }
    if (resolve_play_path(path, resolved, sizeof(resolved)) != STATUS_OK) {
        return STATUS_FAIL;
    }
    st = avi_open(resolved, &ar);
    if (st != STATUS_OK) {
        return st;
    }
    st = avi_read_next_jpeg(&ar, buf, buf_cap, out_len);
    avi_close(&ar);
    return st;
}

status_t lcd_video_play_path(st7789_t *lcd, const char *path)
{
    char resolved[LCD_VIDEO_PATH_MAX];

    if (resolve_play_path(path, resolved, sizeof(resolved)) != STATUS_OK) {
        return STATUS_FAIL;
    }
    return lcd_video_play_internal(lcd, resolved, 0U, false);
}

status_t lcd_video_play_index(st7789_t *lcd, uint8_t idx)
{
    char path[LCD_VIDEO_PATH_MAX];

    if (lcd_video_path_at(idx, path, sizeof(path)) != STATUS_OK) {
        return STATUS_FAIL;
    }
    return lcd_video_play_path(lcd, path);
}

status_t lcd_video_benchmark(st7789_t *lcd, const char *path, uint32_t max_frames)
{
    char     resolved[LCD_VIDEO_PATH_MAX];
    uint32_t n = (max_frames == 0U) ? LCD_VIDEO_BENCH_DEFAULT_FRAMES : max_frames;

    if (resolve_play_path(path, resolved, sizeof(resolved)) != STATUS_OK) {
        return STATUS_FAIL;
    }
    return lcd_video_play_internal(lcd, resolved, n, true);
}
