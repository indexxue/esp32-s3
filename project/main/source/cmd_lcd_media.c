/**
 * @file cmd_lcd_media.c
 * @brief 工程专用厂测命令：LCD BMP/BIN 显示与 SD 视频播放队列。
 */

#include "cmd_lcd_media.h"

#include "board.h"
#include "cmd.h"
#include "lcd_gallery.h"
#include "lcd_video.h"
#include "sdcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cmd_lcdbmp(int argc, const char *argv[])
{
    st7789_t  *lcd;
    status_t   st;
    const char *path;

    (void)argc;
    if (argc < 2) {
        cmd_reply_ng();
        return;
    }
    path = argv[1];
    lcd  = BoardSt7789();

    if (!st7789_is_initialized(lcd)) {
        cmd_reply_ok("lcdbmp", "no_lcd");
        return;
    }
    if (sdcard_get_card() == NULL) {
        cmd_reply_ok("lcdbmp", "no_sd");
        return;
    }

    st = lcd_gallery_show_bmp_path(lcd, path);
    if (st == STATUS_OK) {
        cmd_reply_ok("lcdbmp", "ok");
        return;
    }
    if (st == STATUS_INVALID_ARG) {
        cmd_reply_ok("lcdbmp", "bad_bmp");
        return;
    }
    if (st == STATUS_NOT_SUPPORTED) {
        cmd_reply_ok("lcdbmp", "not_supported");
        return;
    }
    if (st == STATUS_NO_MEM) {
        cmd_reply_ok("lcdbmp", "no_mem");
        return;
    }
    cmd_reply_ok("lcdbmp", "fail");
}

static void cmd_lcdshow(int argc, const char *argv[])
{
    st7789_t  *lcd;
    status_t   st;
    const char *path;
    uint8_t    ix;

    (void)argc;
    if (argc < 2) {
        cmd_reply_ng();
        return;
    }
    path = argv[1];
    lcd  = BoardSt7789();

    if (!st7789_is_initialized(lcd)) {
        cmd_reply_ok("lcdshow", "no_lcd");
        return;
    }
    if (sdcard_get_card() == NULL) {
        cmd_reply_ok("lcdshow", "no_sd");
        return;
    }

    st = lcd_gallery_show_path(lcd, path);
    if (st == STATUS_OK) {
        const char *bn = strrchr(path, '/');

        bn = (bn != NULL) ? (bn + 1) : path;
        ix = lcd_gallery_find_index_by_basename(bn);
        if (ix != LCD_GALLERY_INDEX_NONE) {
            lcd_gallery_set_current_index(ix);
        }
        cmd_reply_ok("lcdshow", "ok");
        return;
    }
    if (st == STATUS_INVALID_ARG) {
        cmd_reply_ok("lcdshow", "bad_file");
        return;
    }
    if (st == STATUS_NOT_SUPPORTED) {
        cmd_reply_ok("lcdshow", "not_supported");
        return;
    }
    if (st == STATUS_NO_MEM) {
        cmd_reply_ok("lcdshow", "no_mem");
        return;
    }
    cmd_reply_ok("lcdshow", "fail");
}

static void cmd_videolist(int argc, const char *argv[])
{
    char val[CMD_STATUS_BUF_SIZE];
    int  n;

    (void)argc;
    (void)argv;

    if (sdcard_get_card() == NULL) {
        cmd_reply_ok("video", "no_sd");
        return;
    }
    (void)lcd_video_scan();
    n = (int)lcd_video_count();
    if (n <= 0) {
        cmd_reply_ok("video", "empty");
        return;
    }
    val[0] = '\0';
    for (int i = 0; i < n; i++) {
        char        path[320];
        const char *bn;

        if (lcd_video_path_at((uint8_t)i, path, sizeof(path)) != STATUS_OK) {
            continue;
        }
        bn = strrchr(path, '/');
        bn = (bn != NULL) ? (bn + 1) : path;
        if (val[0] != '\0') {
            (void)strncat(val, ",", sizeof(val) - strlen(val) - 1U);
        }
        (void)strncat(val, bn, sizeof(val) - strlen(val) - 1U);
    }
    cmd_reply_ok("video", val);
}

static void cmd_videoplay(int argc, const char *argv[])
{
    if (argc < 2) {
        cmd_reply_ng();
        return;
    }
    if (sdcard_get_card() == NULL) {
        cmd_reply_ok("video", "no_sd");
        return;
    }
    if (!lcd_video_post_play_path(argv[1])) {
        cmd_reply_ok("video", "busy");
        return;
    }
    cmd_reply_ok("video", "queued");
}

static void cmd_videoplay_i(int argc, const char *argv[])
{
    long ix;

    if (argc < 2) {
        cmd_reply_ng();
        return;
    }
    if (sdcard_get_card() == NULL) {
        cmd_reply_ok("video", "no_sd");
        return;
    }
    ix = strtol(argv[1], NULL, 10);
    if ((ix < 0) || (ix > 255)) {
        cmd_reply_ok("video", "bad_idx");
        return;
    }
    if (!lcd_video_post_play_index((uint8_t)ix)) {
        cmd_reply_ok("video", "busy");
        return;
    }
    cmd_reply_ok("video", "queued");
}

static void cmd_videostop(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    lcd_video_request_stop();
    cmd_reply_ok("video", "stop");
}

static void cmd_videobench(int argc, const char *argv[])
{
    const char *path;
    uint32_t    frames = 100U;

    if (argc < 2) {
        cmd_reply_ng();
        return;
    }
    path = argv[1];
    if (argc >= 3) {
        long n = strtol(argv[2], NULL, 10);
        if ((n > 0) && (n <= 10000)) {
            frames = (uint32_t)n;
        }
    }
    if (sdcard_get_card() == NULL) {
        cmd_reply_ok("video", "no_sd");
        return;
    }
    if (!BoardSt7789() || !st7789_is_initialized(BoardSt7789())) {
        cmd_reply_ok("video", "no_lcd");
        return;
    }
    if (!lcd_video_post_benchmark(path, frames)) {
        cmd_reply_ok("video", "busy");
        return;
    }
    cmd_reply_ok("video", "bench_queued");
}

void cmd_lcd_media_register(void)
{
    (void)cmd_register("lcdbmp", cmd_lcdbmp, "show BMP on LCD: lcdbmp <absolute_path>");
    (void)cmd_register("lcdshow", cmd_lcdshow, "show BMP or BIN on LCD: lcdshow <absolute_path>");
    (void)cmd_register("video", cmd_videolist, "list videos under /sdcard/videos");
    (void)cmd_register("videoplay", cmd_videoplay, "queue play: videoplay <name|path>");
    (void)cmd_register("videoplay_i", cmd_videoplay_i, "queue play by index: videoplay_i <n>");
    (void)cmd_register("videostop", cmd_videostop, "request stop video playback");
    (void)cmd_register("videobench", cmd_videobench, "FPS bench: videobench <name|path> [frames]");
}
