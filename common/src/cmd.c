/**
 * @file    cmd.c
 * @brief   厂测命令行框架实现（对齐 STM32 `serial_cmd.c` 的解析与应答格式）。
 */

#include "cmd.h"

#include "board.h"
#include "i2c.h"
#include "lcd.h"
#include "lcd_gallery.h"
#include "led_scene.h"
#include "log.h"
#include "persist.h"
#include "boot_slot.h"
#include "sdcard.h"
#include "usb_serial_jtag.h"

#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cmd_write_fn s_write_fn;
static void *s_write_ctx;
static cmd_entry_t s_cmd_table[CMD_MAX_COMMANDS];
static int s_cmd_count;
static SemaphoreHandle_t s_uart_mutex;
static TaskHandle_t s_reader_task;
static cmd_web_capture_t *s_web_capture;
/** 当前行原始文本（`trim_and_tokenize` 前，保留 SSID/密码大小写），供 `webcfg` 解析。 */
static char s_cmd_raw_line[CMD_LINE_MAX];

static nvs_web_ctrl_settings_t s_webcfg_stage;
static bool s_webcfg_stage_valid;
static bool s_webcfg_stage_dirty;

static bool cmd_write_discard(const void *data, size_t len, void *user_ctx)
{
    (void)data;
    (void)len;
    (void)user_ctx;
    return true;
}

static bool cmd_write_stdout(const void *data, size_t len, void *user_ctx)
{
    (void)user_ctx;
    if ((data == NULL) || (len == 0U)) {
        return true;
    }
    size_t w = fwrite(data, 1U, len, stdout);
    (void)fflush(stdout);
    return (w == len);
}

static bool cmd_write_usb(const void *data, size_t len, void *user_ctx)
{
    usize_t written = 0U;

    (void)user_ctx;
    if ((data == NULL) || (len == 0U)) {
        return true;
    }
    if (UsbSerialJtagWrite((const u8_t *)data, (usize_t)len, (s32_t)CMD_TX_MUTEX_TIMEOUT_MS, &written) != TRUE) {
        return false;
    }
    return (written == (usize_t)len);
}

void cmd_init(cmd_write_fn write_fn, void *write_ctx)
{
    s_write_fn = (write_fn != NULL) ? write_fn : cmd_write_stdout;
    s_write_ctx = write_ctx;
    s_cmd_count = 0;
    (void)memset(s_cmd_table, 0, sizeof(s_cmd_table));
    if (s_uart_mutex == NULL) {
        s_uart_mutex = xSemaphoreCreateRecursiveMutex();
    }
}

int cmd_register(const char *name, cmd_handler_t handler, const char *help)
{
    size_t nlen;

    if ((name == NULL) || (handler == NULL) || (s_cmd_count >= (int)CMD_MAX_COMMANDS)) {
        return -1;
    }
    nlen = strlen(name);
    if ((nlen == 0U) || (nlen >= CMD_NAME_MAX)) {
        return -1;
    }
    (void)strncpy(s_cmd_table[s_cmd_count].name, name, CMD_NAME_MAX - 1U);
    s_cmd_table[s_cmd_count].name[CMD_NAME_MAX - 1U] = '\0';
    s_cmd_table[s_cmd_count].handler = handler;
    if (help != NULL) {
        (void)strncpy(s_cmd_table[s_cmd_count].help, help, CMD_HELP_MAX - 1U);
        s_cmd_table[s_cmd_count].help[CMD_HELP_MAX - 1U] = '\0';
    } else {
        s_cmd_table[s_cmd_count].help[0] = '\0';
    }
    s_cmd_count++;
    return 0;
}

static bool cmd_send_unlocked(const uint8_t *data, uint16_t len)
{
    if ((data == NULL) || (len == 0U)) {
        return false;
    }
    if (s_web_capture != NULL) {
        cmd_web_capture_t *c = s_web_capture;

        if ((c->buf == NULL) || (c->cap <= 1U)) {
            return false;
        }
        if (c->len >= c->cap - 1U) {
            return false;
        }
        {
            size_t room = (c->cap - 1U) - c->len;
            size_t n     = ((size_t)len <= room) ? (size_t)len : room;

            (void)memcpy(c->buf + c->len, data, n);
            c->len += n;
            c->buf[c->len] = '\0';
            return (n == (size_t)len);
        }
    }
    if (s_write_fn == NULL) {
        return false;
    }
    return s_write_fn(data, (size_t)len, s_write_ctx);
}

bool cmd_send(const uint8_t *data, uint16_t len)
{
    bool ok;

    if ((data == NULL) || (len == 0U)) {
        return false;
    }
    if (s_uart_mutex != NULL) {
        if (xSemaphoreTakeRecursive(s_uart_mutex, pdMS_TO_TICKS(CMD_TX_MUTEX_TIMEOUT_MS)) != pdTRUE) {
            return false;
        }
    }
    ok = cmd_send_unlocked(data, len);
    if (s_uart_mutex != NULL) {
        (void)xSemaphoreGiveRecursive(s_uart_mutex);
    }
    return ok;
}

void cmd_uart_lock(void)
{
    if (s_uart_mutex != NULL) {
        (void)xSemaphoreTakeRecursive(s_uart_mutex, portMAX_DELAY);
    }
}

void cmd_uart_unlock(void)
{
    if (s_uart_mutex != NULL) {
        (void)xSemaphoreGiveRecursive(s_uart_mutex);
    }
}

bool cmd_send_str(const char *str)
{
    size_t len;

    if (str == NULL) {
        return false;
    }
    len = strlen(str);
    if (len == 0U) {
        return true;
    }
    if (len > 0xFFFFU) {
        return false;
    }
    return cmd_send((const uint8_t *)str, (uint16_t)len);
}

void cmd_reply_ok(const char *cmd, const char *value)
{
    char buf[CMD_LINE_MAX + 16U];
    int n;

    n = snprintf(buf, sizeof(buf), "%s:%s\r\n", (cmd != NULL) ? cmd : "", (value != NULL) ? value : "");
    if ((n > 0) && ((size_t)n < sizeof(buf))) {
        (void)cmd_send((const uint8_t *)buf, (uint16_t)n);
    }
}

void cmd_reply_ng(void)
{
    (void)cmd_send_str("ng\r\n");
}

static void trim_and_tokenize(char *line, const char *argv[], int *argc, int max_argc)
{
    *argc = 0;
    while ((*line == ' ') || (*line == '\t')) {
        line++;
    }
    while ((*line != '\0') && (*argc < max_argc)) {
        argv[*argc] = line;
        (*argc)++;
        while ((*line != '\0') && (*line != ' ') && (*line != '\t') && (*line != '\r') && (*line != '\n')) {
            *line = (char)tolower((unsigned char)*line);
            line++;
        }
        if (*line == '\0') {
            break;
        }
        *line = '\0';
        line++;
        while ((*line == ' ') || (*line == '\t')) {
            line++;
        }
    }
}

static void do_help(void)
{
    char buf[CMD_STATUS_BUF_SIZE];
    int n;

    n = snprintf(buf, sizeof(buf), "help: list commands and usage\r\n");
    if ((n > 0) && ((size_t)n < sizeof(buf))) {
        (void)cmd_send((const uint8_t *)buf, (uint16_t)n);
    }
    for (int i = 0; i < s_cmd_count; i++) {
        const char *h = s_cmd_table[i].help;
        if (h[0] == '\0') {
            h = "";
        }
        n = snprintf(buf, sizeof(buf), "%s: %s\r\n", s_cmd_table[i].name, h);
        if ((n > 0) && ((size_t)n < sizeof(buf))) {
            (void)cmd_send((const uint8_t *)buf, (uint16_t)n);
        }
    }
}

static void cmd_process_line_locked(const char *line)
{
    char buf[CMD_LINE_MAX];
    size_t len;
    const char *argv[CMD_MAX_ARGC];
    int argc = 0;

    if (line == NULL) {
        cmd_reply_ng();
        return;
    }
    len = strlen(line);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1U;
    }
    (void)memcpy(buf, line, len);
    buf[len] = '\0';
    for (size_t i = 0U; i < len; i++) {
        if ((buf[i] == '\r') || (buf[i] == '\n')) {
            buf[i] = '\0';
            break;
        }
    }
    {
        char *space_pos = strchr(buf, ' ');
        char *tab_pos = strchr(buf, '\t');
        char *sep = strchr(buf, ':');
        char *first_blank = space_pos;
        if ((first_blank == NULL) || ((tab_pos != NULL) && (tab_pos < first_blank))) {
            first_blank = tab_pos;
        }
        if ((sep != NULL) && ((first_blank == NULL) || (sep < first_blank))) {
            *sep = ' ';
        }
    }
    {
        size_t raw_len = strlen(buf);
        if (raw_len >= sizeof(s_cmd_raw_line)) {
            raw_len = sizeof(s_cmd_raw_line) - 1U;
        }
        (void)memcpy(s_cmd_raw_line, buf, raw_len);
        s_cmd_raw_line[raw_len] = '\0';
    }
    trim_and_tokenize(buf, argv, &argc, (int)CMD_MAX_ARGC);
    if (argc == 0) {
        cmd_reply_ng();
        return;
    }
    if (strcmp(argv[0], "help") == 0) {
        do_help();
        return;
    }
    for (int i = 0; i < s_cmd_count; i++) {
        if (strcmp(argv[0], s_cmd_table[i].name) == 0) {
            s_cmd_table[i].handler(argc, argv);
            return;
        }
    }
    cmd_reply_ng();
}

void cmd_process_line(const char *line)
{
    if (s_uart_mutex != NULL) {
        (void)xSemaphoreTakeRecursive(s_uart_mutex, portMAX_DELAY);
    }
    cmd_process_line_locked(line);
    if (s_uart_mutex != NULL) {
        (void)xSemaphoreGiveRecursive(s_uart_mutex);
    }
}

void cmd_process_line_for_web(const char *line, cmd_web_capture_t *cap)
{
    if ((cap == NULL) || (cap->buf == NULL) || (cap->cap == 0U)) {
        return;
    }
    if (s_uart_mutex != NULL) {
        (void)xSemaphoreTakeRecursive(s_uart_mutex, portMAX_DELAY);
    }
    s_web_capture = cap;
    cap->len = 0;
    cap->buf[0] = '\0';
    cmd_process_line_locked(line);
    s_web_capture = NULL;
    if (cap->len < cap->cap) {
        cap->buf[cap->len] = '\0';
    } else if (cap->cap > 0U) {
        cap->buf[cap->cap - 1U] = '\0';
    }
    if (s_uart_mutex != NULL) {
        (void)xSemaphoreGiveRecursive(s_uart_mutex);
    }
}

/* ---------- 默认命令（与 STM32 厂测子集对齐） ---------- */

static void cmd_sn(int argc, const char *argv[])
{
    if (argc == 1) {
        char sn[NVS_SN_SIZE];
        if (nvs_sn_get(sn)) {
            cmd_reply_ok("sn", sn);
        } else {
            cmd_reply_ng();
        }
        return;
    }
    if (argc == 2) {
        const char *value = argv[1];
        size_t slen = strlen(value);
        if ((slen >= 10U) && (slen <= 12U) && nvs_sn_set(value)) {
            cmd_reply_ok("sn", "set");
            return;
        }
        cmd_reply_ng();
        return;
    }
    cmd_reply_ng();
}

static void cmd_devtype(int argc, const char *argv[])
{
    char b[8];

    (void)argc;
    (void)argv;
    (void)snprintf(b, sizeof(b), "%u", (unsigned int)nvs_device_type_get());
    cmd_reply_ok("devtype", b);
}

static void cmd_mac(int argc, const char *argv[])
{
    if (argc == 1) {
        uint8_t mac[NVS_MAC_SIZE];
        char out[32];
        if (nvs_mac_get(mac)) {
            (void)snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                           mac[4], mac[5], mac[6], mac[7]);
            cmd_reply_ok("mac", out);
        } else {
            cmd_reply_ng();
        }
        return;
    }
    if (argc == 2) {
        const char *value = argv[1];
        size_t vlen = strlen(value);
        if (vlen == (size_t)NVS_MAC_SIZE) {
            uint8_t mac[NVS_MAC_SIZE];
            bool ok = true;
            for (size_t i = 0U; i < (size_t)NVS_MAC_SIZE; i++) {
                if ((value[i] < '0') || (value[i] > '9')) {
                    ok = false;
                    break;
                }
                mac[i] = (uint8_t)(value[i] - '0');
            }
            if (ok && nvs_mac_set(mac)) {
                cmd_reply_ok("mac", "set");
                return;
            }
        }
        cmd_reply_ng();
        return;
    }
    cmd_reply_ng();
}

static void cmd_led(int argc, const char *argv[])
{
    bool all = false;
    led_scene_led_e led = LED_SCENE_LED_MAX_NUM;

    if (argc < 3) {
        cmd_reply_ng();
        return;
    }
    if (strcmp(argv[1], "r") == 0) {
        led = LED_SCENE_LED_0;
    } else if (strcmp(argv[1], "b") == 0) {
        led = LED_SCENE_LED_1;
    } else if (strcmp(argv[1], "all") == 0) {
        all = true;
    } else {
        cmd_reply_ng();
        return;
    }
    if (strcmp(argv[2], "on") == 0) {
        if (all) {
            led_scene_led_direct_set(LED_SCENE_LED_0, true);
            led_scene_led_direct_set(LED_SCENE_LED_1, true);
        } else {
            led_scene_led_direct_set(led, true);
        }
        cmd_reply_ok("led", "ok");
        return;
    }
    if (strcmp(argv[2], "off") == 0) {
        if (all) {
            led_scene_led_direct_set(LED_SCENE_LED_0, false);
            led_scene_led_direct_set(LED_SCENE_LED_1, false);
        } else {
            led_scene_led_direct_set(led, false);
        }
        cmd_reply_ok("led", "ok");
        return;
    }
    cmd_reply_ng();
}

static void cmd_reboot(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    cmd_reply_ok("reboot", "now");
    (void)UsbSerialJtagWaitTxDone((s32_t)200);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

static void cmd_boot_slot_reply_and_reset(const char *cmd_name, status_t st)
{
    if (st != STATUS_OK) {
        LOG_ERROR("cmd %s: %s", (cmd_name != NULL) ? cmd_name : "boot", status_to_str(st));
        cmd_reply_ng();
        return;
    }
    cmd_reply_ok(cmd_name, "restart");
    (void)UsbSerialJtagWaitTxDone((s32_t)200);
    vTaskDelay(pdMS_TO_TICKS(50));
    boot_slot_system_reset();
}

static void cmd_boot_a(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    cmd_boot_slot_reply_and_reset("boot_a", boot_slot_request_app_a());
}

static void cmd_boot_b(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    cmd_boot_slot_reply_and_reset("boot_b", boot_slot_request_factory());
}

static void cmd_boot_q(int argc, const char *argv[])
{
    char buf[CMD_STATUS_BUF_SIZE];

    (void)argc;
    (void)argv;
    if (boot_slot_format_status(buf, sizeof(buf)) != STATUS_OK) {
        cmd_reply_ng();
        return;
    }
    cmd_reply_ok("boot_q", buf);
}

static void cmd_log(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    LOG_INFO("[CMD] usb cmd log test ok");
    cmd_reply_ok("log", "ok");
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    s32_t port;
} cmd_i2c_scan_ctx_t;

static void cmd_i2c_scan_cb(void *user_ctx, u16_t address7bit)
{
    cmd_i2c_scan_ctx_t *c = (cmd_i2c_scan_ctx_t *)user_ctx;
    int w;

    if ((c == NULL) || (c->buf == NULL) || (c->cap <= c->len + 1U)) {
        return;
    }
    w = snprintf(c->buf + c->len, c->cap - c->len, "%s%d:0x%02X", (c->len > 0U) ? "," : "", (int)c->port,
                 (unsigned int)address7bit);
    if ((w > 0) && ((size_t)w < (c->cap - c->len))) {
        c->len += (size_t)w;
    }
}

static void cmd_i2c(int argc, const char *argv[])
{
    char buf[CMD_STATUS_BUF_SIZE];
    cmd_i2c_scan_ctx_t ctx = {buf, sizeof(buf), 0U, 0};

    (void)argc;
    (void)argv;
    for (s32_t p = 0; p < (s32_t)BSP_I2C_HW_PORT_COUNT; p++) {
        ctx.port = p;
        (void)I2cScanBus7Bit(p, cmd_i2c_scan_cb, &ctx);
    }
    if (ctx.len == 0U) {
        (void)strncpy(buf, "none", sizeof(buf) - 1U);
        buf[sizeof(buf) - 1U] = '\0';
    }
    cmd_reply_ok("i2c", buf);
}

static void cmd_version(int argc, const char *argv[])
{
    char ver[NVS_APP_VERSION_SIZE];

    (void)argc;
    (void)argv;
    if (nvs_app_version_get(ver)) {
        cmd_reply_ok("version", ver);
    } else {
        cmd_reply_ok("version", "?");
    }
}

/** 与原先 `main.c` 中 `APP_LCD_REFRESH_BENCH_FRAMES` 默认一致；可用 `lcdbench <n>` 覆盖。 */
#define CMD_LCD_BENCH_FRAMES_DEFAULT (40U)
#define CMD_LCD_BENCH_FRAMES_MAX (200U)

static void cmd_lcdbench(int argc, const char *argv[])
{
    st7789_t *lcd = BoardSt7789();
    uint16_t w;
    uint16_t h;
    int64_t t0;
    int64_t dt_us;
    int n;
    double sec;
    double fps_row;
    double fps_bulk;
    uint32_t px;
    double mb_s;
    char val[CMD_LINE_MAX];
    unsigned long frames = CMD_LCD_BENCH_FRAMES_DEFAULT;

    if (argc >= 2) {
        char *end = NULL;
        frames = strtoul(argv[1], &end, 10);
        if ((end == argv[1]) || (frames < 1UL) || (frames > (unsigned long)CMD_LCD_BENCH_FRAMES_MAX)) {
            cmd_reply_ng();
            return;
        }
    }

    if (!st7789_is_initialized(lcd)) {
        cmd_reply_ok("lcdbench", "no_lcd");
        return;
    }

    w = st7789_display_width(lcd);
    h = st7789_display_height(lcd);
    px = (uint32_t)w * (uint32_t)h;
    n = (int)frames;

    t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        lcd_fill(lcd, 0U, 0U, w, h, (uint16_t)((i & 1) != 0 ? LCD_COLOR_RED : LCD_COLOR_BLUE));
    }
    dt_us = esp_timer_get_time() - t0;
    sec = (dt_us > 0) ? ((double)dt_us / 1000000.0) : 1.0;
    fps_row = (double)n / sec;

    t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        lcd_fill_fast(lcd, 0U, 0U, w, h, (uint16_t)((i & 1) != 0 ? LCD_COLOR_GREEN : LCD_COLOR_MAGENTA));
    }
    dt_us = esp_timer_get_time() - t0;
    sec = (dt_us > 0) ? ((double)dt_us / 1000000.0) : 1.0;
    fps_bulk = (double)n / sec;
    mb_s = ((double)px * 2.0 * fps_bulk) / (1024.0 * 1024.0);

    LOG_INFO(
        "ST7789 DMA bench: %d full frames | row lcd_fill %.2f FPS | bulk lcd_fill_fast %.2f FPS (~%.2f MiB/s pixel "
        "stream) | SPI %u Hz",
        n,
        fps_row,
        fps_bulk,
        mb_s,
        (unsigned int)BOARD_ST7789_SPI_CLOCK_HZ);

    (void)snprintf(val, sizeof(val), "n=%d r=%.1f b=%.1f m=%.2f spi%u", n, fps_row, fps_bulk, mb_s,
                   (unsigned int)BOARD_ST7789_SPI_CLOCK_HZ);
    val[sizeof(val) - 1U] = '\0';
    cmd_reply_ok("lcdbench", val);
}

static void cmd_lcdbmp(int argc, const char *argv[])
{
    st7789_t *lcd;
    status_t  st;
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
    st7789_t *lcd;
    status_t  st;
    const char *path;
    uint8_t   ix;

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

static void cmd_sdtest(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    if (sdcard_get_card() == NULL) {
        cmd_reply_ok("sdtest", "no_card");
        return;
    }

    sdcard_mount_smoke_and_benchmark_log();
    cmd_reply_ok("sdtest", "ok");
}

static int ascii_strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0U; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if ((ca == '\0') && (cb == '\0')) {
            return 0;
        }
        if ((ca == '\0') || (cb == '\0')) {
            return (int)ca - (int)cb;
        }
        ca = (unsigned char)tolower((int)ca);
        cb = (unsigned char)tolower((int)cb);
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
    }
    return 0;
}

static const char *webcfg_skip_ws(const char *p)
{
    while ((*p == ' ') || (*p == '\t')) {
        p++;
    }
    return p;
}

static const char *webcfg_after_header(const char *raw)
{
    const char *p = webcfg_skip_ws(raw);
    const char want[] = "webcfg";
    const size_t n = sizeof(want) - 1U;

    if (ascii_strncasecmp(p, want, n) != 0) {
        return NULL;
    }
    return webcfg_skip_ws(p + n);
}

static bool webcfg_payload_starts_with_ci(const char *p, const char *lit)
{
    size_t n = strlen(lit);

    if (ascii_strncasecmp(p, lit, n) != 0) {
        return false;
    }
    return (p[n] == '\0') || isspace((unsigned char)p[n]);
}

static void webcfg_stage_ensure_init(void)
{
    if (s_webcfg_stage_valid) {
        return;
    }
    if (nvs_web_ctrl_settings_get(&s_webcfg_stage)) {
        s_webcfg_stage_valid = true;
    } else {
        nvs_web_ctrl_settings_default(&s_webcfg_stage);
        s_webcfg_stage_valid = true;
    }
}

static void webcfg_stage_reset_ram(void)
{
    s_webcfg_stage_valid = false;
    s_webcfg_stage_dirty = false;
}

static const char *webcfg_after_sta(const char *raw)
{
    const char *p = webcfg_after_header(raw);

    if (p == NULL) {
        return NULL;
    }
    if (!webcfg_payload_starts_with_ci(p, "sta")) {
        return NULL;
    }
    p += 3;
    return webcfg_skip_ws(p);
}

/** 串口写入待保存的 STA（路由器）SSID/密码；与网页 `POST /api/wifi/save` 写入同一 NVS 字段。 */
static void cmd_webcfg_sta_from_raw(const char *raw)
{
    const char *p = webcfg_after_sta(raw);

    if ((p == NULL) || (*p == '\0')) {
        cmd_reply_ng();
        return;
    }

    webcfg_stage_ensure_init();
    {
        nvs_web_ctrl_settings_t tmp = s_webcfg_stage;
        const char *ssid_s = p;

        while ((*p != '\0') && !isspace((unsigned char)*p)) {
            p++;
        }
        {
            const size_t ssid_len = (size_t)(p - ssid_s);

            if ((ssid_len == 0U) || (ssid_len > 32U)) {
                cmd_reply_ng();
                return;
            }
            (void)snprintf(tmp.sta_ssid, sizeof(tmp.sta_ssid), "%.*s", (int)ssid_len, ssid_s);
        }

        p = webcfg_skip_ws(p);
        if (*p != '\0') {
            const char *pass_s = p;

            while ((*p != '\0') && !isspace((unsigned char)*p)) {
                p++;
            }
            {
                const size_t pass_len = (size_t)(p - pass_s);

                if ((pass_len == 1U) && (pass_s[0] == '-')) {
                    tmp.sta_password[0] = '\0';
                } else {
                    if (pass_len > 64U) {
                        cmd_reply_ng();
                        return;
                    }
                    (void)snprintf(tmp.sta_password, sizeof(tmp.sta_password), "%.*s", (int)pass_len, pass_s);
                }
            }
        }

        p = webcfg_skip_ws(p);
        if (*p != '\0') {
            cmd_reply_ng();
            return;
        }

        tmp.magic = NVS_WEB_CTRL_MAGIC;
        if (!nvs_web_ctrl_settings_validate(&tmp)) {
            cmd_reply_ng();
            return;
        }
        s_webcfg_stage       = tmp;
        s_webcfg_stage_valid = true;
        s_webcfg_stage_dirty = true;
        cmd_reply_ok("webcfg", "staged_ok");
    }
}

static void cmd_webcfg_set_from_raw(const char *raw)
{
    const char *p = webcfg_after_header(raw);

    if (p == NULL) {
        cmd_reply_ng();
        return;
    }
    if (!webcfg_payload_starts_with_ci(p, "set")) {
        cmd_reply_ng();
        return;
    }
    p += 3;
    p = webcfg_skip_ws(p);
    if (*p == '\0') {
        cmd_reply_ng();
        return;
    }

    webcfg_stage_ensure_init();
    {
        nvs_web_ctrl_settings_t tmp = s_webcfg_stage;
        const char *ssid_s = p;

        while ((*p != '\0') && !isspace((unsigned char)*p)) {
            p++;
        }
        {
            const size_t ssid_len = (size_t)(p - ssid_s);

            if ((ssid_len == 0U) || (ssid_len > 32U)) {
                cmd_reply_ng();
                return;
            }
            (void)snprintf(tmp.softap_ssid, sizeof(tmp.softap_ssid), "%.*s", (int)ssid_len, ssid_s);
        }

        p = webcfg_skip_ws(p);
        if (*p != '\0') {
            const char *pass_s = p;

            while ((*p != '\0') && !isspace((unsigned char)*p)) {
                p++;
            }
            {
                const size_t pass_len = (size_t)(p - pass_s);

                if ((pass_len == 1U) && (pass_s[0] == '-')) {
                    tmp.softap_password[0] = '\0';
                } else {
                    if (pass_len > 64U) {
                        cmd_reply_ng();
                        return;
                    }
                    (void)snprintf(tmp.softap_password, sizeof(tmp.softap_password), "%.*s", (int)pass_len, pass_s);
                }
            }
        }

        p = webcfg_skip_ws(p);
        if (*p != '\0') {
            cmd_reply_ng();
            return;
        }

        tmp.magic = NVS_WEB_CTRL_MAGIC;
        if (!nvs_web_ctrl_settings_validate(&tmp)) {
            cmd_reply_ng();
            return;
        }
        s_webcfg_stage       = tmp;
        s_webcfg_stage_valid = true;
        s_webcfg_stage_dirty = true;
        cmd_reply_ok("webcfg", "staged_ok");
    }
}

static void cmd_webcfg_tune(int argc, const char *argv[])
{
    char *end = NULL;
    unsigned long u;

    if ((argc != 3) && (argc != 4) && (argc != 5)) {
        cmd_reply_ng();
        return;
    }

    webcfg_stage_ensure_init();
    {
        nvs_web_ctrl_settings_t tmp = s_webcfg_stage;

        u = strtoul(argv[2], &end, 10);
        if ((end == argv[2]) || (*end != '\0') || (u > 13UL)) {
            cmd_reply_ng();
            return;
        }
        tmp.softap_channel = (uint8_t)u;

        if (argc >= 4) {
            u = strtoul(argv[3], &end, 10);
            if ((end == argv[3]) || (*end != '\0') || (u > 10UL)) {
                cmd_reply_ng();
                return;
            }
            tmp.softap_max_connection = (uint8_t)u;
        }
        if (argc >= 5) {
            u = strtoul(argv[4], &end, 10);
            if ((end == argv[4]) || (*end != '\0') || (u > 65535UL)) {
                cmd_reply_ng();
                return;
            }
            tmp.http_port = (uint16_t)u;
        }

        tmp.magic = NVS_WEB_CTRL_MAGIC;
        if (!nvs_web_ctrl_settings_validate(&tmp)) {
            cmd_reply_ng();
            return;
        }
        s_webcfg_stage       = tmp;
        s_webcfg_stage_valid = true;
        s_webcfg_stage_dirty = true;
        cmd_reply_ok("webcfg", "staged_ok");
    }
}

/** `webcfg save` 失败时仍以前缀 `ng` 开头，便于脚本判断；后缀说明原因。 */
static void cmd_webcfg_reply_save_ng(const char *reason)
{
    char b[72];
    int  n;

    n = snprintf(b, sizeof(b), "ng save:%s\r\n", reason);
    if ((n > 0) && ((size_t)n < sizeof(b))) {
        (void)cmd_send_str(b);
    } else {
        cmd_reply_ng();
    }
}

static void cmd_webcfg_show(void)
{
    nvs_web_ctrl_settings_t st;
    const char *src = "builtin";
    char out[CMD_LINE_MAX];
    int n;

    if (s_webcfg_stage_dirty && s_webcfg_stage_valid) {
        st   = s_webcfg_stage;
        src  = "staged";
    } else if (nvs_web_ctrl_settings_get(&st)) {
        src = "nvs";
    } else {
        nvs_web_ctrl_settings_default(&st);
    }

    n = snprintf(out, sizeof(out), "src=%s ap_ssid=%s ch=%u max=%u port=%u pass=%s sta_ssid=%s", src, st.softap_ssid,
                 (unsigned int)st.softap_channel, (unsigned int)st.softap_max_connection, (unsigned int)st.http_port,
                 (st.softap_password[0] != '\0') ? st.softap_password : "-",
                 (st.sta_ssid[0] != '\0') ? st.sta_ssid : "-");
    if ((n <= 0) || ((size_t)n >= sizeof(out))) {
        cmd_reply_ng();
        return;
    }
    cmd_reply_ok("webcfg", out);
}

static void cmd_webcfg(int argc, const char *argv[])
{
    if (argc <= 1) {
        cmd_webcfg_show();
        return;
    }
    if (strcmp(argv[1], "show") == 0) {
        cmd_webcfg_show();
        return;
    }
    if (strcmp(argv[1], "sta") == 0) {
        cmd_webcfg_sta_from_raw(s_cmd_raw_line);
        return;
    }
    if (strcmp(argv[1], "set") == 0) {
        cmd_webcfg_set_from_raw(s_cmd_raw_line);
        return;
    }
    if (strcmp(argv[1], "tune") == 0) {
        cmd_webcfg_tune(argc, argv);
        return;
    }
    if (strcmp(argv[1], "save") == 0) {
        /* 保证内存里有一份与 NVS/默认一致的基线（与是否已暂存修改无关）。 */
        webcfg_stage_ensure_init();
        if ((!s_webcfg_stage_dirty) || (!s_webcfg_stage_valid)) {
            cmd_webcfg_reply_save_ng("nothing_staged_run_sta_or_set_or_tune_first");
            return;
        }
        if (!nvs_web_ctrl_settings_validate(&s_webcfg_stage)) {
            cmd_webcfg_reply_save_ng("invalid_cfg");
            return;
        }
        if (!nvs_web_ctrl_settings_set(&s_webcfg_stage)) {
            cmd_webcfg_reply_save_ng("nvs_write_failed");
            return;
        }
        cmd_reply_ok("webcfg", "saved_reboot");
        (void)UsbSerialJtagWaitTxDone((s32_t)200);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
        return;
    }
    if (strcmp(argv[1], "reset") == 0) {
        (void)nvs_web_ctrl_settings_delete();
        webcfg_stage_reset_ram();
        cmd_reply_ok("webcfg", "cleared_reboot");
        (void)UsbSerialJtagWaitTxDone((s32_t)200);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
        return;
    }
    if (strcmp(argv[1], "discard") == 0) {
        webcfg_stage_reset_ram();
        cmd_reply_ok("webcfg", "discarded");
        return;
    }
    cmd_reply_ng();
}

void cmd_register_defaults(void)
{
    (void)cmd_register("sn", cmd_sn, "get/set serial (set: 10..12 chars)");
    (void)cmd_register("devtype", cmd_devtype, "read device type byte");
    (void)cmd_register("mac", cmd_mac, "get/set mac blob (set: 8 decimal digits)");
    (void)cmd_register("led", cmd_led, "led r|b|all on|off");
    (void)cmd_register("reboot", cmd_reboot, "software reset");
    (void)cmd_register("boot_a", cmd_boot_a, "next boot app_a (ota_0) + reset");
    (void)cmd_register("boot_b", cmd_boot_b, "next boot app_b (ota_1) + reset");
    (void)cmd_register("boot_q", cmd_boot_q, "query run/next partition labels");
    (void)cmd_register("log", cmd_log, "emit one log line + ok");
    (void)cmd_register("i2c", cmd_i2c, "scan I2C0..1 (port:addr)");
    (void)cmd_register("version", cmd_version, "app version string from NVS");
    (void)cmd_register("lcdbench", cmd_lcdbench, "ST7789 SPI DMA fill bench [frames 1-200]");
    (void)cmd_register("lcdbmp", cmd_lcdbmp, "show BMP on LCD: lcdbmp <absolute_path>");
    (void)cmd_register("lcdshow", cmd_lcdshow, "show BMP or BIN on LCD: lcdshow <absolute_path>");
    (void)cmd_register("sdtest", cmd_sdtest, "SD FAT smoke + DMA throughput log");
    (void)cmd_register("webcfg", cmd_webcfg,
                       "wifi: sta,set,tune then save (need staged_ok)");
}

void cmd_embed_register_defaults_if_needed(void)
{
    if (s_cmd_count > 0) {
        return;
    }
    cmd_init(cmd_write_discard, NULL);
    cmd_register_defaults();
}

#define CMD_READER_STACK_WORDS (4096U)
#define CMD_READER_PRIORITY (3U)
#define CMD_READER_POLL_MS (50)

static void cmd_reader_task(void *arg)
{
    char line[CMD_LINE_MAX];
    size_t li = 0U;

    (void)arg;
    for (;;) {
        u8_t ch;
        usize_t nread = 0U;

        if ((UsbSerialJtagRead(&ch, 1U, (s32_t)CMD_READER_POLL_MS, &nread) != TRUE) || (nread == 0U)) {
            continue;
        }
        if ((ch == '\b') || (ch == 0x7FU)) {
            if (li > 0U) {
                li--;
            }
            continue;
        }
        if ((ch == '\r') || (ch == '\n')) {
            if (li > 0U) {
                line[li] = '\0';
                cmd_process_line(line);
                li = 0U;
            }
            continue;
        }
        if (li < (sizeof(line) - 1U)) {
            line[li++] = (char)ch;
        }
    }
}

status_t cmd_usb_line_service_start(void)
{
    UsbSerialJtagDriverConfig_t cfg = {.txBufferSize = 512U, .rxBufferSize = 256U};

    if (s_reader_task != NULL) {
        return STATUS_OK;
    }
    if (UsbSerialJtagDriverInit(&cfg) != TRUE) {
        LOG_ERROR("cmd: UsbSerialJtagDriverInit failed err=%d", (int)UsbSerialJtagGetLastError());
        return STATUS_FAIL;
    }
    cmd_init(cmd_write_usb, NULL);
    cmd_register_defaults();
    if (xTaskCreate(cmd_reader_task, "cmd_usb", CMD_READER_STACK_WORDS, NULL, CMD_READER_PRIORITY, &s_reader_task) !=
        pdPASS) {
        LOG_ERROR("cmd: create reader task failed");
        s_reader_task = NULL;
        return STATUS_FAIL;
    }
    LOG_INFO("cmd: USB line reader started (factory commands, type help)");
    return STATUS_OK;
}
