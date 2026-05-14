/**
 * @file lcd_gallery.c
 * @brief SD 图库：`.bin`（RGB565 裸或 RGBH 头）与 `.bmp`（BI_RGB 24/32 位），条带写 ST7789。
 */

#include "lcd_gallery.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "board.h"
#include "dma.h"
#include "log.h"
#include "sdcard.h"
#include "st7789.h"

#define LCD_GALLERY_RGB565_FULL_BYTES (64800U)
#define LCD_GALLERY_BIN_HDR_BYTES     (16U)
#define LCD_GALLERY_HDR_FLAG_ORDER_COLUMN (1u << 0u)

#define LCD_GALLERY_MAX_FILES (24U)

/** `BOARD_SDCARD_MOUNT_POINT` + `/` + 长文件名（VFAT LFN 常见上限 255）+ 余量 */
#define LCD_GALLERY_PATH_MAX (320U)

#define BMP_MAX_ROW_STRIDE (32768U)

static char    s_paths[LCD_GALLERY_MAX_FILES][LCD_GALLERY_PATH_MAX];
static uint8_t s_count;
static uint8_t s_index;

static bool path_suffix_icase(const char *path, const char *suf)
{
    size_t lp;
    size_t ls;

    if (path == NULL || suf == NULL) {
        return false;
    }
    lp = strlen(path);
    ls = strlen(suf);
    if (lp < ls) {
        return false;
    }
    return strcasecmp(path + (lp - ls), suf) == 0;
}

static bool bin_hdr_valid(const uint8_t *hdr, uint16_t expect_w, uint16_t expect_h)
{
    uint16_t hw;
    uint16_t hh;
    uint32_t flags;

    if (memcmp(hdr, "RGBH", 4) != 0) {
        LOG_WARN("lcd_gallery: bad RGBH header magic");
        return false;
    }
    hw = (uint16_t)((uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8));
    hh = (uint16_t)((uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8));
    if ((hw != expect_w) || (hh != expect_h)) {
        LOG_WARN("lcd_gallery: header %ux%u != panel %ux%u", (unsigned int)hw, (unsigned int)hh,
                 (unsigned int)expect_w, (unsigned int)expect_h);
        return false;
    }
    flags = (uint32_t)((uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24));
    if ((flags & LCD_GALLERY_HDR_FLAG_ORDER_COLUMN) != 0U) {
        LOG_WARN("lcd_gallery: column-major bin not supported");
        return false;
    }
    return true;
}

static uint32_t rgb565_strip_bytes(uint16_t dst_w)
{
    uint32_t row_b = (uint32_t)dst_w * 2U;
    uint32_t strip_max = (uint32_t)BOARD_ST7789_SPI_MAX_TX;
    strip_max = (strip_max / row_b) * row_b;
    if (strip_max == 0U) {
        strip_max = row_b;
    }
    return strip_max;
}

static status_t stream_rgb565_payload(st7789_t *lcd, uint16_t dst_w, uint16_t dst_h, FILE *fp, uint32_t payload_bytes)
{
    const uint32_t need = (uint32_t)dst_w * (uint32_t)dst_h * 2U;
    uint32_t       strip_max;
    void          *buf;
    uint32_t       remain;
    size_t         got;

    if (payload_bytes != need) {
        LOG_WARN("lcd_gallery: rgb565 payload %u need %u", (unsigned int)payload_bytes, (unsigned int)need);
        return STATUS_INVALID_ARG;
    }

    strip_max = rgb565_strip_bytes(dst_w);
    buf       = DmaMalloc((usize_t)strip_max);
    if (buf == NULL_PTR) {
        LOG_ERROR("lcd_gallery: DmaMalloc strip %u failed", (unsigned int)strip_max);
        return STATUS_NO_MEM;
    }

    if (st7789_set_window(lcd, 0U, 0U, (uint16_t)(dst_w - 1U), (uint16_t)(dst_h - 1U)) != ST7789_OK) {
        DmaFree(buf);
        return STATUS_FAIL;
    }

    remain = need;
    while (remain > 0U) {
        size_t chunk = (remain > strip_max) ? (size_t)strip_max : (size_t)remain;

        got = fread(buf, 1U, chunk, fp);
        if (got != chunk) {
            st7789_end_write(lcd);
            DmaFree(buf);
            LOG_WARN("lcd_gallery: rgb565 fread %u need %u", (unsigned int)got, (unsigned int)chunk);
            return STATUS_FAIL;
        }
        if (st7789_write_pixel_bytes(lcd, (const uint8_t *)buf, (uint32_t)got) != ST7789_OK) {
            st7789_end_write(lcd);
            DmaFree(buf);
            return STATUS_FAIL;
        }
        remain -= (uint32_t)got;
    }

    st7789_end_write(lcd);
    DmaFree(buf);
    return STATUS_OK;
}

static status_t show_rgb565_bin_file(st7789_t *lcd, uint16_t dst_w, uint16_t dst_h, const char *path)
{
    const uint32_t need = (uint32_t)dst_w * (uint32_t)dst_h * 2U;
    FILE          *fp;
    long           fsize;
    uint32_t       payload = need;
    long           body_off = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        LOG_WARN("lcd_gallery: fopen %s errno %d", path, (int)errno);
        return STATUS_FAIL;
    }

    (void)fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);

    if (fsize == (long)need + (long)LCD_GALLERY_BIN_HDR_BYTES) {
        uint8_t hdr[LCD_GALLERY_BIN_HDR_BYTES];

        if (fread(hdr, 1U, (size_t)LCD_GALLERY_BIN_HDR_BYTES, fp) != (size_t)LCD_GALLERY_BIN_HDR_BYTES) {
            (void)fclose(fp);
            return STATUS_FAIL;
        }
        if (!bin_hdr_valid(hdr, dst_w, dst_h)) {
            (void)fclose(fp);
            return STATUS_FAIL;
        }
        body_off = (long)LCD_GALLERY_BIN_HDR_BYTES;
    } else if (fsize != (long)need) {
        (void)fclose(fp);
        LOG_WARN("lcd_gallery: %s size %ld expect %u or %u", path, fsize, (unsigned int)need,
                 (unsigned int)(need + LCD_GALLERY_BIN_HDR_BYTES));
        return STATUS_FAIL;
    }

    if (body_off != 0) {
        (void)fseek(fp, body_off, SEEK_SET);
    }

    {
        status_t st = stream_rgb565_payload(lcd, dst_w, dst_h, fp, payload);
        (void)fclose(fp);
        return st;
    }
}

static void pack_rgb565_be_bgr_panel(const uint8_t *bgr, uint8_t *dst2)
{
    uint8_t b = bgr[0];
    uint8_t g = bgr[1];
    uint8_t r = bgr[2];
    uint8_t tmp = r;
    r           = b;
    b           = tmp;
    {
        uint16_t p = (uint16_t)(((uint16_t)(r & 0xF8U) << 8) | ((uint16_t)(g & 0xFCU) << 3) | ((uint16_t)b >> 3));
        dst2[0]     = (uint8_t)(p >> 8);
        dst2[1]     = (uint8_t)(p & 0xFFU);
    }
}

static uint32_t bmp_map_src_y(uint32_t dst_y, uint32_t abs_h, uint16_t dst_h, bool top_down)
{
    uint32_t t;
    if (abs_h <= 1U) {
        return 0U;
    }
    if (dst_h <= 1U) {
        return 0U;
    }
    t = (uint32_t)(((uint64_t)dst_y * (uint64_t)(abs_h - 1U)) / (uint64_t)((uint32_t)dst_h - 1U));
    if (!top_down) {
        return (abs_h - 1U) - t;
    }
    return t;
}

static uint32_t bmp_map_src_x(uint32_t dst_x, uint32_t bmp_w, uint16_t dst_w)
{
    if (bmp_w <= 1U) {
        return 0U;
    }
    if (dst_w <= 1U) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)dst_x * (uint64_t)(bmp_w - 1U)) / (uint64_t)((uint32_t)dst_w - 1U));
}

static status_t show_bmp_file(st7789_t *lcd, uint16_t dst_w, uint16_t dst_h, const char *path)
{
    FILE          *fp = NULL;
    uint8_t        file54[54];
    size_t         got;
    uint32_t       offbits;
    int32_t        biw;
    int32_t        bih;
    uint16_t       planes;
    uint16_t       bitcount;
    uint32_t       comp;
    uint32_t       bmp_w;
    uint32_t       abs_h;
    bool           top_down;
    uint32_t       bpp;
    uint32_t       row_stride;
    uint32_t       strip_max;
    uint8_t       *blk = NULL;
    uint8_t       *rowb;
    uint8_t       *stripb;
    usize_t        blk_sz;
    uint32_t       y;
    uint32_t       out_used = 0U;
    const uint32_t row_out  = (uint32_t)dst_w * 2U;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        LOG_WARN("lcd_gallery: bmp fopen %s errno %d", path, (int)errno);
        return STATUS_FAIL;
    }

    got = fread(file54, 1U, sizeof(file54), fp);
    if (got != sizeof(file54)) {
        (void)fclose(fp);
        LOG_WARN("lcd_gallery: bmp short header %s", path);
        return STATUS_FAIL;
    }
    if ((file54[0] != 0x42U) || (file54[1] != 0x4DU)) {
        (void)fclose(fp);
        LOG_WARN("lcd_gallery: not a BM file %s", path);
        return STATUS_FAIL;
    }

    offbits = (uint32_t)file54[10] | ((uint32_t)file54[11] << 8) | ((uint32_t)file54[12] << 16) | ((uint32_t)file54[13] << 24);
    biw     = (int32_t)((int32_t)file54[18] | ((int32_t)file54[19] << 8) | ((int32_t)file54[20] << 16) | ((int32_t)file54[21] << 24));
    bih     = (int32_t)((int32_t)file54[22] | ((int32_t)file54[23] << 8) | ((int32_t)file54[24] << 16) | ((int32_t)file54[25] << 24));
    planes  = (uint16_t)((uint16_t)file54[26] | ((uint16_t)file54[27] << 8));
    bitcount = (uint16_t)((uint16_t)file54[28] | ((uint16_t)file54[29] << 8));
    comp    = (uint32_t)file54[30] | ((uint32_t)file54[31] << 8) | ((uint32_t)file54[32] << 16) | ((uint32_t)file54[33] << 24);

    if ((biw <= 0) || (bih == 0)) {
        (void)fclose(fp);
        LOG_WARN("lcd_gallery: bmp invalid dimensions");
        return STATUS_INVALID_ARG;
    }

    if ((planes != 1U) || (comp != 0U)) {
        (void)fclose(fp);
        LOG_WARN("lcd_gallery: bmp only BI_RGB uncompressed (planes=%u comp=%u)", (unsigned int)planes,
                 (unsigned int)comp);
        return STATUS_NOT_SUPPORTED;
    }

    if ((bitcount != 24U) && (bitcount != 32U)) {
        (void)fclose(fp);
        LOG_WARN("lcd_gallery: bmp bitcount %u (need 24 or 32)", (unsigned int)bitcount);
        return STATUS_NOT_SUPPORTED;
    }

    bmp_w = (uint32_t)biw;
    if (bih < 0) {
        if (bih == INT32_MIN) {
            (void)fclose(fp);
            return STATUS_INVALID_ARG;
        }
        abs_h    = (uint32_t)(-bih);
        top_down = true;
    } else {
        abs_h    = (uint32_t)bih;
        top_down = false;
    }

    bpp = (uint32_t)bitcount / 8U;
    row_stride = (bmp_w * bpp + 3U) & ~3U;
    if ((row_stride == 0U) || (row_stride > BMP_MAX_ROW_STRIDE)) {
        (void)fclose(fp);
        LOG_WARN("lcd_gallery: bmp row_stride %u too large", (unsigned int)row_stride);
        return STATUS_INVALID_ARG;
    }

    strip_max = rgb565_strip_bytes(dst_w);
    blk_sz    = (usize_t)row_stride + (usize_t)strip_max;
    blk       = (uint8_t *)DmaMalloc(blk_sz);
    if (blk == NULL) {
        (void)fclose(fp);
        LOG_ERROR("lcd_gallery: bmp DmaMalloc %u failed", (unsigned int)blk_sz);
        return STATUS_NO_MEM;
    }
    rowb   = blk;
    stripb = blk + row_stride;

    if (st7789_set_window(lcd, 0U, 0U, (uint16_t)(dst_w - 1U), (uint16_t)(dst_h - 1U)) != ST7789_OK) {
        DmaFree(blk);
        (void)fclose(fp);
        return STATUS_FAIL;
    }

    for (y = 0U; y < (uint32_t)dst_h; y++) {
        uint32_t sy;
        long     pos;

        if (out_used + row_out > strip_max) {
            if (st7789_write_pixel_bytes(lcd, stripb, out_used) != ST7789_OK) {
                st7789_end_write(lcd);
                DmaFree(blk);
                (void)fclose(fp);
                return STATUS_FAIL;
            }
            out_used = 0U;
        }

        sy = bmp_map_src_y(y, abs_h, dst_h, top_down);
        pos = (long)offbits + (long)sy * (long)row_stride;
        if (fseek(fp, pos, SEEK_SET) != 0) {
            st7789_end_write(lcd);
            DmaFree(blk);
            (void)fclose(fp);
            LOG_WARN("lcd_gallery: bmp fseek fail");
            return STATUS_FAIL;
        }
        if (fread(rowb, 1U, (size_t)row_stride, fp) != (size_t)row_stride) {
            st7789_end_write(lcd);
            DmaFree(blk);
            (void)fclose(fp);
            LOG_WARN("lcd_gallery: bmp row fread fail");
            return STATUS_FAIL;
        }

        {
            uint16_t ox;
            uint8_t *rowdst = stripb + out_used;

            for (ox = 0U; ox < dst_w; ox++) {
                uint32_t sx = bmp_map_src_x((uint32_t)ox, bmp_w, dst_w);
                if (bitcount == 24U) {
                    pack_rgb565_be_bgr_panel(&rowb[sx * 3U], rowdst + (size_t)ox * 2U);
                } else {
                    pack_rgb565_be_bgr_panel(&rowb[sx * 4U], rowdst + (size_t)ox * 2U);
                }
            }
        }
        out_used += row_out;
    }

    if (out_used > 0U) {
        if (st7789_write_pixel_bytes(lcd, stripb, out_used) != ST7789_OK) {
            st7789_end_write(lcd);
            DmaFree(blk);
            (void)fclose(fp);
            return STATUS_FAIL;
        }
    }

    st7789_end_write(lcd);
    DmaFree(blk);
    (void)fclose(fp);
    return STATUS_OK;
}

static void gallery_sort_paths(void)
{
    uint8_t i;
    uint8_t j;

    for (i = 0U; i + 1U < s_count; i++) {
        for (j = (uint8_t)(i + 1U); j < s_count; j++) {
            if (strcmp(s_paths[i], s_paths[j]) > 0) {
                char tmp[LCD_GALLERY_PATH_MAX];
                (void)memcpy(tmp, s_paths[i], sizeof(tmp));
                (void)memcpy(s_paths[i], s_paths[j], sizeof(s_paths[i]));
                (void)memcpy(s_paths[j], tmp, sizeof(s_paths[j]));
            }
        }
    }
}

void lcd_gallery_rescan(void)
{
    DIR           *dir;
    struct dirent *ent;

    s_count = 0U;
    if (sdcard_get_card() == NULL) {
        return;
    }

    dir = opendir(BOARD_SDCARD_MOUNT_POINT);
    if (dir == NULL) {
        LOG_WARN("lcd_gallery: opendir %s failed errno %d", BOARD_SDCARD_MOUNT_POINT, (int)errno);
        return;
    }

    while ((ent = readdir(dir)) != NULL) {
        struct stat st;

        if (ent->d_name[0] == 0) {
            continue;
        }
        if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
            continue;
        }
        if (!path_suffix_icase(ent->d_name, ".bin") && !path_suffix_icase(ent->d_name, ".bmp")) {
            continue;
        }
        if (s_count >= LCD_GALLERY_MAX_FILES) {
            break;
        }
        (void)snprintf(s_paths[s_count], sizeof(s_paths[s_count]), "%s/%s", BOARD_SDCARD_MOUNT_POINT, ent->d_name);
        if (stat(s_paths[s_count], &st) != 0) {
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        s_count++;
    }
    (void)closedir(dir);

    gallery_sort_paths();
    if (s_count > 0U) {
        if (s_index >= s_count) {
            s_index = 0U;
        }
        LOG_INFO("lcd_gallery: %u files under %s", (unsigned int)s_count, BOARD_SDCARD_MOUNT_POINT);
    } else {
        LOG_WARN("lcd_gallery: no .bin/.bmp under %s", BOARD_SDCARD_MOUNT_POINT);
    }
}

uint8_t lcd_gallery_count(void)
{
    return s_count;
}

uint8_t lcd_gallery_current(void)
{
    if (s_count == 0U) {
        return 0U;
    }
    return s_index;
}

void lcd_gallery_next(void)
{
    if (s_count == 0U) {
        return;
    }
    s_index = (uint8_t)((s_index + 1U) % s_count);
}

status_t lcd_gallery_show_index(st7789_t *lcd, uint8_t idx)
{
    uint16_t dst_w;
    uint16_t dst_h;
    status_t st;

    if ((lcd == NULL) || !st7789_is_initialized(lcd)) {
        return STATUS_INVALID_STATE;
    }
    if (sdcard_get_card() == NULL) {
        return STATUS_INVALID_STATE;
    }
    if (s_count == 0U) {
        LOG_WARN("lcd_gallery: empty file list");
        return STATUS_FAIL;
    }

    s_index = (uint8_t)(idx % s_count);
    dst_w   = st7789_display_width(lcd);
    dst_h   = st7789_display_height(lcd);
    if (((uint32_t)dst_w * (uint32_t)dst_h * 2U) != LCD_GALLERY_RGB565_FULL_BYTES) {
        LOG_ERROR("lcd_gallery: panel not 240x135 full-frame mode");
        return STATUS_INVALID_ARG;
    }

    if (path_suffix_icase(s_paths[s_index], ".bmp")) {
        st = show_bmp_file(lcd, dst_w, dst_h, s_paths[s_index]);
    } else {
        st = show_rgb565_bin_file(lcd, dst_w, dst_h, s_paths[s_index]);
    }

    if (st == STATUS_OK) {
        LOG_INFO("lcd_gallery: showing [%u/%u] %s", (unsigned int)s_index + 1U, (unsigned int)s_count, s_paths[s_index]);
    }
    return st;
}
