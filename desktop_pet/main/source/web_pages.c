/**
 * @file web_pages.c
 * @brief desktop_pet：`GET /` 与 SD 卡浏览 / 上传 / 下载 / 建目录 / 删除 HTTP API。
 *
 * AP / STA 共用同一 httpd；路径相对 `BOARD_SDCARD_MOUNT_POINT`，禁止 `..`。
 * 皮肤 zip 上传见 `web_skin.c`（`POST /api/pet/skin`）。
 */

#include "web_pages.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "sdcard.h"
#include "ui.h"
#include "web_skin.h"

#include "esp_http_server.h"
#include "esp_log.h"

#define WEB_SD_PATH_MAX       (200U)
#define WEB_SD_NAME_MAX       (120U)
#define WEB_SD_REL_MAX        (160U)
#define WEB_SD_MAX_FILES      (80U)
#define WEB_SD_JSON_MAX       (16384U)
#define WEB_SD_SEND_CHUNK     (1024U)
#define WEB_SD_RECV_CHUNK     (2048U)
#define WEB_SD_UPLOAD_MAX     (8U * 1024U * 1024U) /* 8 MiB：字库 / 帧图等 */

static const char *TAG = "web_pages";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

typedef struct {
    char          name[WEB_SD_NAME_MAX + 1U];
    unsigned long size_bytes;
    bool          is_dir;
} web_sd_entry_t;

esp_err_t web_pages_root_get_handler(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);

    (void)httpd_resp_set_type(req, "text/html; charset=utf-8");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, len);
}

static void discard_post_remainder(httpd_req_t *req)
{
    size_t consumed = 0U;
    size_t cl;

    if (req->content_len <= 0) {
        return;
    }
    cl = (size_t)req->content_len;
    while (consumed < cl) {
        char   b[256];
        size_t ask = cl - consumed;
        int    n;

        if (ask > sizeof(b)) {
            ask = sizeof(b);
        }
        n = httpd_req_recv(req, b, ask);
        if (n <= 0) {
            break;
        }
        consumed += (size_t)n;
    }
}

static int web_sd_hex_nibble(char c)
{
    if ((c >= '0') && (c <= '9')) {
        return c - '0';
    }
    if ((c >= 'A') && (c <= 'F')) {
        return 10 + (c - 'A');
    }
    if ((c >= 'a') && (c <= 'f')) {
        return 10 + (c - 'a');
    }
    return -1;
}

/** `httpd_query_key_value` 不解码；浏览器会把 `/` 编成 `%2F`。 */
static void web_sd_url_decode_inplace(char *s)
{
    size_t r = 0U;
    size_t w = 0U;

    if (s == NULL) {
        return;
    }
    while (s[r] != '\0') {
        if ((s[r] == '%') && (s[r + 1U] != '\0') && (s[r + 2U] != '\0')) {
            const int hi = web_sd_hex_nibble(s[r + 1U]);
            const int lo = web_sd_hex_nibble(s[r + 2U]);

            if ((hi >= 0) && (lo >= 0)) {
                s[w++] = (char)((hi << 4) | lo);
                r += 3U;
                continue;
            }
        }
        if (s[r] == '+') {
            s[w++] = ' ';
            r++;
            continue;
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
}

/** 读 `path=` 查询参数并 URL 解码；无参数时输出空串。 */
static esp_err_t web_sd_query_path(httpd_req_t *req, char *rel, size_t rel_cap)
{
    char qry[256];

    if ((rel == NULL) || (rel_cap == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    rel[0] = '\0';
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (httpd_query_key_value(qry, "path", rel, rel_cap) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    web_sd_url_decode_inplace(rel);
    return ESP_OK;
}

/** 相对路径：可为空；禁止 `/` 开头、`..`、非法字符。 */
static bool web_sd_relpath_ok(const char *rel)
{
    size_t i;
    size_t len;
    size_t seg = 0U;

    if (rel == NULL) {
        return false;
    }
    len = strlen(rel);
    if (len > WEB_SD_REL_MAX) {
        return false;
    }
    if ((len > 0U) && ((rel[0] == '/') || (rel[len - 1U] == '/'))) {
        return false;
    }
    for (i = 0U; i < len; i++) {
        const unsigned char c = (unsigned char)rel[i];

        if (c == '/') {
            if ((seg == 0U) || (seg > WEB_SD_NAME_MAX)) {
                return false;
            }
            if ((seg == 2U) && (rel[i - 2U] == '.') && (rel[i - 1U] == '.')) {
                return false;
            }
            if ((seg == 1U) && (rel[i - 1U] == '.')) {
                return false;
            }
            seg = 0U;
            continue;
        }
        if ((isalnum(c) != 0) || (c == '.') || (c == '_') || (c == '-') || (c == '~')) {
            seg++;
            continue;
        }
        return false;
    }
    if (len == 0U) {
        return true;
    }
    if ((seg == 0U) || (seg > WEB_SD_NAME_MAX)) {
        return false;
    }
    if ((seg == 2U) && (rel[len - 2U] == '.') && (rel[len - 1U] == '.')) {
        return false;
    }
    if ((seg == 1U) && (rel[len - 1U] == '.')) {
        return false;
    }
    return true;
}

static bool web_sd_basename_ok(const char *name)
{
    size_t i;
    size_t len;

    if (name == NULL) {
        return false;
    }
    len = strlen(name);
    if ((len == 0U) || (len > WEB_SD_NAME_MAX) || (name[0] == '.')) {
        return false;
    }
    for (i = 0U; i < len; i++) {
        const unsigned char c = (unsigned char)name[i];

        if ((isalnum(c) != 0) || (c == '.') || (c == '_') || (c == '-') || (c == '~')) {
            continue;
        }
        return false;
    }
    return true;
}

static esp_err_t web_sd_build_abs(const char *rel, char *out, size_t out_cap)
{
    int n;

    if ((out == NULL) || (out_cap < 8U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((rel == NULL) || (rel[0] == '\0')) {
        n = snprintf(out, out_cap, "%s", BOARD_SDCARD_MOUNT_POINT);
    } else {
        n = snprintf(out, out_cap, "%s/%s", BOARD_SDCARD_MOUNT_POINT, rel);
    }
    if ((n <= 0) || ((size_t)n >= out_cap)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/** Create each path segment under mount (rel may be "" → no-op). */
static bool web_sd_mkdir_p(const char *rel)
{
    char   acc[WEB_SD_REL_MAX + 1U];
    char   abs[WEB_SD_PATH_MAX];
    size_t i;
    size_t n;
    size_t start = 0U;

    if ((rel == NULL) || (rel[0] == '\0')) {
        return true;
    }
    if (!web_sd_relpath_ok(rel)) {
        return false;
    }
    n = strlen(rel);
    acc[0] = '\0';
    for (i = 0U; i <= n; i++) {
        if ((i < n) && (rel[i] != '/')) {
            continue;
        }
        if (i == start) {
            return false;
        }
        if ((i - start) >= sizeof(acc)) {
            return false;
        }
        if (acc[0] != '\0') {
            size_t al = strlen(acc);

            if ((al + 1U + (i - start)) >= sizeof(acc)) {
                return false;
            }
            acc[al] = '/';
            (void)memcpy(acc + al + 1U, rel + start, i - start);
            acc[al + 1U + (i - start)] = '\0';
        } else {
            (void)memcpy(acc, rel + start, i - start);
            acc[i - start] = '\0';
        }
        if (web_sd_build_abs(acc, abs, sizeof(abs)) != ESP_OK) {
            return false;
        }
        if (mkdir(abs, 0755) != 0) {
            struct stat st;

            if ((errno != EEXIST) || (stat(abs, &st) != 0) || !S_ISDIR(st.st_mode)) {
                return false;
            }
        }
        start = i + 1U;
    }
    return true;
}

static bool web_sd_parent_rel(const char *rel_file, char *parent, size_t parent_cap)
{
    const char *slash;

    if ((rel_file == NULL) || (parent == NULL) || (parent_cap == 0U)) {
        return false;
    }
    slash = strrchr(rel_file, '/');
    if (slash == NULL) {
        parent[0] = '\0';
        return true;
    }
    if ((size_t)(slash - rel_file) >= parent_cap) {
        return false;
    }
    (void)memcpy(parent, rel_file, (size_t)(slash - rel_file));
    parent[slash - rel_file] = '\0';
    return web_sd_relpath_ok(parent);
}

static esp_err_t web_sd_json_err(httpd_req_t *req, const char *http_status, const char *err)
{
    char buf[96];
    int  n;

    (void)httpd_resp_set_status(req, http_status);
    (void)httpd_resp_set_type(req, "application/json");
    n = snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", (err != NULL) ? err : "err");
    if (n <= 0) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"err\"}", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, buf, (size_t)n);
}

static void web_sd_sort_entries(web_sd_entry_t *ents, size_t n)
{
    size_t i;
    size_t j;

    for (i = 0U; i + 1U < n; i++) {
        for (j = i + 1U; j < n; j++) {
            const bool swap =
                (ents[i].is_dir != ents[j].is_dir)
                    ? (!ents[i].is_dir && ents[j].is_dir)
                    : (strcmp(ents[i].name, ents[j].name) > 0);

            if (swap) {
                web_sd_entry_t tmp = ents[i];

                ents[i] = ents[j];
                ents[j] = tmp;
            }
        }
    }
}

static esp_err_t sd_list_get_handler(httpd_req_t *req)
{
    char              rel[WEB_SD_REL_MAX + 1U];
    char              abs[WEB_SD_PATH_MAX];
    DIR              *dir;
    struct dirent    *ent;
    web_sd_entry_t    ents[WEB_SD_MAX_FILES];
    size_t            nents = 0U;
    static char       json[WEB_SD_JSON_MAX];
    size_t            pos = 0U;
    int               w;
    size_t            k;

    rel[0] = '\0';
    (void)web_sd_query_path(req, rel, sizeof(rel));
    if (!web_sd_relpath_ok(rel)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_path\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (sdcard_get_card() == NULL) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_sd\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (web_sd_build_abs(rel, abs, sizeof(abs)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"path\"}", HTTPD_RESP_USE_STRLEN);
    }

    dir = opendir(abs);
    if (dir == NULL) {
        (void)httpd_resp_set_status(req, "404 Not Found");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"opendir\"}", HTTPD_RESP_USE_STRLEN);
    }

    while ((ent = readdir(dir)) != NULL) {
        struct stat st;
        char        full[WEB_SD_PATH_MAX];

        if ((ent->d_name[0] == '\0') || (strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
            continue;
        }
        if (!web_sd_basename_ok(ent->d_name)) {
            continue;
        }
        if (nents >= WEB_SD_MAX_FILES) {
            break;
        }
        if (snprintf(full, sizeof(full), "%s/%s", abs, ent->d_name) >= (int)sizeof(full)) {
            continue;
        }
        if (stat(full, &st) != 0) {
            continue;
        }
        (void)memset(&ents[nents], 0, sizeof(ents[nents]));
        (void)snprintf(ents[nents].name, sizeof(ents[nents].name), "%.*s", (int)WEB_SD_NAME_MAX, ent->d_name);
        ents[nents].is_dir     = S_ISDIR(st.st_mode) != 0;
        ents[nents].size_bytes = ents[nents].is_dir ? 0UL : (unsigned long)st.st_size;
        nents++;
    }
    (void)closedir(dir);

    web_sd_sort_entries(ents, nents);

    w = snprintf(json + pos, sizeof(json) - pos, "{\"ok\":true,\"path\":\"%s\",\"files\":[", rel);
    if ((w <= 0) || ((size_t)w >= sizeof(json) - pos)) {
        goto overflow;
    }
    pos += (size_t)w;

    for (k = 0U; k < nents; k++) {
        const char *kind = ents[k].is_dir ? "dir" : "file";

        if (k > 0U) {
            if (pos + 2U >= sizeof(json)) {
                goto overflow;
            }
            json[pos++] = ',';
        }
        w = snprintf(json + pos, sizeof(json) - pos,
                     "{\"name\":\"%s\",\"bytes\":%lu,\"kind\":\"%s\"}",
                     ents[k].name, ents[k].size_bytes, kind);
        if ((w <= 0) || ((size_t)w >= sizeof(json) - pos)) {
            goto overflow;
        }
        pos += (size_t)w;
    }

    w = snprintf(json + pos, sizeof(json) - pos, "]}");
    if ((w <= 0) || ((size_t)w >= sizeof(json) - pos)) {
        goto overflow;
    }
    pos += (size_t)w;

    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, pos);

overflow:
    (void)httpd_resp_set_status(req, "500 Internal Server Error");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"json_overflow\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sd_file_get_handler(httpd_req_t *req)
{
    char        rel[WEB_SD_REL_MAX + 1U];
    char        abs[WEB_SD_PATH_MAX];
    struct stat st;
    FILE       *fp = NULL;
    esp_err_t   er;
    const char *base;

    if (sdcard_get_card() == NULL) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_sd\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (web_sd_query_path(req, rel, sizeof(rel)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_path\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (!web_sd_relpath_ok(rel) || (rel[0] == '\0')) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_path\"}", HTTPD_RESP_USE_STRLEN);
    }

    base = strrchr(rel, '/');
    base = (base != NULL) ? (base + 1) : rel;
    if (!web_sd_basename_ok(base)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_name\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (web_sd_build_abs(rel, abs, sizeof(abs)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"path\"}", HTTPD_RESP_USE_STRLEN);
    }

    if ((stat(abs, &st) != 0) || !S_ISREG(st.st_mode)) {
        (void)httpd_resp_set_status(req, "404 Not Found");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"not_found\"}", HTTPD_RESP_USE_STRLEN);
    }

    fp = fopen(abs, "rb");
    if (fp == NULL) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"open\"}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_status(req, "200 OK");
    /* octet-stream + attachment：避免浏览器把 .wav 内联播放而不落盘。 */
    (void)httpd_resp_set_type(req, "application/octet-stream");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    {
        /* httpd_resp_set_hdr 只存指针，须在响应完成前保持有效。 */
        static char s_cd[192];

        (void)snprintf(s_cd, sizeof(s_cd), "attachment; filename=\"%s\"", base);
        (void)httpd_resp_set_hdr(req, "Content-Disposition", s_cd);
    }

    for (;;) {
        char   buf[WEB_SD_SEND_CHUNK];
        size_t n = fread(buf, 1U, sizeof(buf), fp);

        if (n == 0U) {
            break;
        }
        er = httpd_resp_send_chunk(req, buf, n);
        if (er != ESP_OK) {
            (void)fclose(fp);
            return er;
        }
    }
    (void)fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t sd_delete_post_handler(httpd_req_t *req)
{
    char        rel[WEB_SD_REL_MAX + 1U];
    char        abs[WEB_SD_PATH_MAX];
    const char *base;
    char        jbuf[WEB_SD_REL_MAX + 48U];
    struct stat st;
    int         ur;

    if (req->content_len > 0) {
        discard_post_remainder(req);
    }

    if (sdcard_get_card() == NULL) {
        return web_sd_json_err(req, "503 Service Unavailable", "no_sd");
    }

    if (web_sd_query_path(req, rel, sizeof(rel)) != ESP_OK) {
        return web_sd_json_err(req, "400 Bad Request", "need_path");
    }
    if (!web_sd_relpath_ok(rel) || (rel[0] == '\0')) {
        return web_sd_json_err(req, "400 Bad Request", "bad_path");
    }

    base = strrchr(rel, '/');
    base = (base != NULL) ? (base + 1) : rel;
    if (!web_sd_basename_ok(base)) {
        return web_sd_json_err(req, "400 Bad Request", "bad_name");
    }

    if (web_sd_build_abs(rel, abs, sizeof(abs)) != ESP_OK) {
        return web_sd_json_err(req, "400 Bad Request", "path");
    }

    if (stat(abs, &st) != 0) {
        return web_sd_json_err(req, "404 Not Found", "not_found");
    }
    if (S_ISDIR(st.st_mode)) {
        ur = rmdir(abs);
        if (ur != 0) {
            return web_sd_json_err(req, "409 Conflict", "rmdir");
        }
    } else if (S_ISREG(st.st_mode)) {
        ur = unlink(abs);
        if (ur != 0) {
            return web_sd_json_err(req, "404 Not Found", "unlink");
        }
    } else {
        return web_sd_json_err(req, "400 Bad Request", "bad_type");
    }

    ESP_LOGI(TAG, "sd delete %s", abs);
    (void)snprintf(jbuf, sizeof(jbuf), "{\"ok\":true,\"path\":\"%s\"}", rel);
    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, jbuf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sd_mkdir_post_handler(httpd_req_t *req)
{
    char rel[WEB_SD_REL_MAX + 1U];
    char abs[WEB_SD_PATH_MAX];
    char jbuf[WEB_SD_REL_MAX + 48U];
    const char *base;

    if (req->content_len > 0) {
        discard_post_remainder(req);
    }
    if (sdcard_get_card() == NULL) {
        return web_sd_json_err(req, "503 Service Unavailable", "no_sd");
    }
    if (web_sd_query_path(req, rel, sizeof(rel)) != ESP_OK) {
        return web_sd_json_err(req, "400 Bad Request", "need_path");
    }
    if (!web_sd_relpath_ok(rel) || (rel[0] == '\0')) {
        return web_sd_json_err(req, "400 Bad Request", "bad_path");
    }
    base = strrchr(rel, '/');
    base = (base != NULL) ? (base + 1) : rel;
    if (!web_sd_basename_ok(base)) {
        return web_sd_json_err(req, "400 Bad Request", "bad_name");
    }
    if (!web_sd_mkdir_p(rel)) {
        return web_sd_json_err(req, "500 Internal Server Error", "mkdir");
    }
    if (web_sd_build_abs(rel, abs, sizeof(abs)) != ESP_OK) {
        return web_sd_json_err(req, "400 Bad Request", "path");
    }
    ESP_LOGI(TAG, "sd mkdir %s", abs);
    (void)snprintf(jbuf, sizeof(jbuf), "{\"ok\":true,\"path\":\"%s\"}", rel);
    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, jbuf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sd_upload_post_handler(httpd_req_t *req)
{
    char        rel[WEB_SD_REL_MAX + 1U];
    char        parent[WEB_SD_REL_MAX + 1U];
    char        abs[WEB_SD_PATH_MAX];
    char        jbuf[WEB_SD_REL_MAX + 64U];
    const char *base;
    FILE       *fp = NULL;
    size_t      total;
    size_t      got = 0U;

    if (sdcard_get_card() == NULL) {
        discard_post_remainder(req);
        return web_sd_json_err(req, "503 Service Unavailable", "no_sd");
    }
    if (web_sd_query_path(req, rel, sizeof(rel)) != ESP_OK) {
        discard_post_remainder(req);
        return web_sd_json_err(req, "400 Bad Request", "need_path");
    }
    if (!web_sd_relpath_ok(rel) || (rel[0] == '\0')) {
        discard_post_remainder(req);
        return web_sd_json_err(req, "400 Bad Request", "bad_path");
    }
    base = strrchr(rel, '/');
    base = (base != NULL) ? (base + 1) : rel;
    if (!web_sd_basename_ok(base)) {
        discard_post_remainder(req);
        return web_sd_json_err(req, "400 Bad Request", "bad_name");
    }
    if (req->content_len <= 0) {
        return web_sd_json_err(req, "411 Length Required", "need_content_length");
    }
    total = (size_t)req->content_len;
    if (total > WEB_SD_UPLOAD_MAX) {
        discard_post_remainder(req);
        return web_sd_json_err(req, "413 Payload Too Large", "too_large");
    }
    if (!web_sd_parent_rel(rel, parent, sizeof(parent))) {
        discard_post_remainder(req);
        return web_sd_json_err(req, "400 Bad Request", "bad_parent");
    }
    if (!web_sd_mkdir_p(parent)) {
        discard_post_remainder(req);
        return web_sd_json_err(req, "500 Internal Server Error", "mkdir_parent");
    }
    if (web_sd_build_abs(rel, abs, sizeof(abs)) != ESP_OK) {
        discard_post_remainder(req);
        return web_sd_json_err(req, "400 Bad Request", "path");
    }

    fp = fopen(abs, "wb");
    if (fp == NULL) {
        discard_post_remainder(req);
        return web_sd_json_err(req, "500 Internal Server Error", "open");
    }

    while (got < total) {
        char   buf[WEB_SD_RECV_CHUNK];
        size_t ask = total - got;
        int    n;

        if (ask > sizeof(buf)) {
            ask = sizeof(buf);
        }
        n = httpd_req_recv(req, buf, ask);
        if (n <= 0) {
            (void)fclose(fp);
            (void)unlink(abs);
            return web_sd_json_err(req, "400 Bad Request", "recv");
        }
        if (fwrite(buf, 1U, (size_t)n, fp) != (size_t)n) {
            (void)fclose(fp);
            (void)unlink(abs);
            discard_post_remainder(req);
            return web_sd_json_err(req, "500 Internal Server Error", "write");
        }
        got += (size_t)n;
    }
    (void)fclose(fp);

    ESP_LOGI(TAG, "sd upload %s (%u B)", abs, (unsigned)total);
    (void)snprintf(jbuf, sizeof(jbuf), "{\"ok\":true,\"path\":\"%s\",\"bytes\":%u}", rel, (unsigned)total);
    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, jbuf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_send_json(httpd_req_t *req, const char *json)
{
    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t touch_calib_get_handler(httpd_req_t *req)
{
    bool running = false;
    bool saved = false;
    uint8_t step = 0U;
    uint8_t steps = 0U;
    char buf[128];

    desktop_pet_ui_touch_calib_status(&running, &step, &steps, &saved);
    (void)snprintf(buf, sizeof(buf),
                   "{\"ok\":true,\"running\":%s,\"step\":%u,\"steps\":%u,\"saved\":%s}",
                   running ? "true" : "false", (unsigned)step, (unsigned)steps,
                   saved ? "true" : "false");
    return web_send_json(req, buf);
}

static esp_err_t touch_calib_start_post_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        discard_post_remainder(req);
    }
    if (!desktop_pet_ui_touch_calib_start()) {
        (void)httpd_resp_set_status(req, "409 Conflict");
        return web_send_json(req, "{\"ok\":false,\"error\":\"start_failed\"}");
    }
    return web_send_json(req, "{\"ok\":true,\"running\":true}");
}

static esp_err_t touch_calib_reset_post_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        discard_post_remainder(req);
    }
    if (!desktop_pet_ui_touch_calib_reset()) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        return web_send_json(req, "{\"ok\":false,\"error\":\"reset_failed\"}");
    }
    return web_send_json(req, "{\"ok\":true,\"saved\":false}");
}

esp_err_t web_pages_sd_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        {.uri = "/api/sd/list", .method = HTTP_GET, .handler = sd_list_get_handler, .user_ctx = NULL},
        {.uri = "/api/sd/file", .method = HTTP_GET, .handler = sd_file_get_handler, .user_ctx = NULL},
        {.uri = "/api/sd/upload", .method = HTTP_POST, .handler = sd_upload_post_handler, .user_ctx = NULL},
        {.uri = "/api/sd/mkdir", .method = HTTP_POST, .handler = sd_mkdir_post_handler, .user_ctx = NULL},
        {.uri = "/api/sd/delete", .method = HTTP_POST, .handler = sd_delete_post_handler, .user_ctx = NULL},
        {.uri = "/api/touch/calib", .method = HTTP_GET, .handler = touch_calib_get_handler, .user_ctx = NULL},
        {.uri = "/api/touch/calib/start", .method = HTTP_POST, .handler = touch_calib_start_post_handler,
         .user_ctx = NULL},
        {.uri = "/api/touch/calib/reset", .method = HTTP_POST, .handler = touch_calib_reset_post_handler,
         .user_ctx = NULL},
    };
    size_t i;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (i = 0U; i < (sizeof(uris) / sizeof(uris[0])); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "SD file API registered");
    return web_skin_http_register(server);
}
