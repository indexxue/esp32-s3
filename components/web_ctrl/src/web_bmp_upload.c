/**
 * @file web_bmp_upload.c
 * @brief BMP 上传与 SD 图库 HTTP：multipart 上传、`GET /api/gallery/list`、BMP 预览、删除后 `lcd_gallery_rescan`。
 */

#include "web_bmp_upload.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdkconfig.h"

#include "board.h"
#include "cmd.h"
#include "lcd_gallery.h"
#include "lcd_video.h"
#include "persist.h"
#include "sdcard.h"
#include "web_ctrl_cmd.h"

#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#ifndef CONFIG_WEB_CTRL_BMP_UPLOAD_MAX
#define WEB_CTRL_BMP_UPLOAD_MAX_BYTES (1048576U)
#else
#define WEB_CTRL_BMP_UPLOAD_MAX_BYTES ((size_t)CONFIG_WEB_CTRL_BMP_UPLOAD_MAX)
#endif

#ifndef CONFIG_WEB_CTRL_BMP_CMD_TIMEOUT_MS
#define WEB_CTRL_BMP_CMD_TIMEOUT_MS (25000)
#else
#define WEB_CTRL_BMP_CMD_TIMEOUT_MS (CONFIG_WEB_CTRL_BMP_CMD_TIMEOUT_MS)
#endif

/** 根目录短名（≤8.3），避免 FAT 未开 LFN 时 `fopen` 报 EINVAL；每次上传 `W`+7 位十六进制。 */

#define WEB_BMP_PATH_MAX (160U)
/** 与 `lcd_gallery` 根目录扫描上限一致 */
#define WEB_GALLERY_MAX_FILES (24U)
#define WEB_GALLERY_NAME_MAX  (120U)
#define WEB_GALLERY_JSON_MAX  (8192U)
#define GALLERY_BMP_SEND_CHUNK (1024U)
#define MP_STASH_MAX     (2048U)
#define MP_RECV_CHUNK    (1024U)
#define BOUNDARY_MAX     (80U)
#define ENDMARK_EXTRA    (8U)

static const char *TAG = "web_bmp_upload";

static void discard_post_remainder(httpd_req_t *req, size_t consumed)
{
    size_t cl;

    if (req->content_len <= 0) {
        return;
    }
    cl = (size_t)req->content_len;
    if (consumed >= cl) {
        return;
    }
    while (consumed < cl) {
        uint8_t b[256];
        size_t ask = cl - consumed;
        int    r;

        if (ask > sizeof(b)) {
            ask = sizeof(b);
        }
        r = httpd_req_recv(req, (char *)b, ask);
        if (r <= 0) {
            break;
        }
        consumed += (size_t)r;
    }
}

static const uint8_t *find_bytes(const uint8_t *hay, size_t hn, const uint8_t *needle, size_t nn)
{
    size_t i;

    if ((needle == NULL) || (nn == 0U) || (hay == NULL) || (hn < nn)) {
        return NULL;
    }
    for (i = 0U; i + nn <= hn; i++) {
        if (memcmp(hay + i, needle, nn) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

static const uint8_t *find_header_body_separator(const uint8_t *buf, size_t len)
{
    const uint8_t *p;

    p = find_bytes(buf, len, (const uint8_t *)"\r\n\r\n", 4U);
    if (p != NULL) {
        return p;
    }
    return find_bytes(buf, len, (const uint8_t *)"\n\n", 2U);
}

static size_t header_body_sep_len(const uint8_t *sep)
{
    if ((sep != NULL) && (sep[0] == '\r')) {
        return 4U;
    }
    return 2U;
}

static bool stash_headers_have_file_field(const uint8_t *hdr, size_t hdr_len)
{
    static const uint8_t n1[] = "name=\"file\"";
    static const uint8_t n2[] = "name='file'";
    size_t i;

    if (find_bytes(hdr, hdr_len, n1, sizeof(n1) - 1U) != NULL) {
        return true;
    }
    if (find_bytes(hdr, hdr_len, n2, sizeof(n2) - 1U) != NULL) {
        return true;
    }
    /* name=file 或 name = "file"（忽略 name 与引号间空白） */
    for (i = 0U; i + 6U < hdr_len; i++) {
        if (strncasecmp((const char *)(hdr + i), "name=", 5) != 0) {
            continue;
        }
        {
            size_t j = i + 5U;

            while ((j < hdr_len) && ((hdr[j] == (uint8_t)' ') || (hdr[j] == (uint8_t)'\t'))) {
                j++;
            }
            if (j >= hdr_len) {
                continue;
            }
            if (hdr[j] == (uint8_t)'"') {
                j++;
                if ((j + 4U <= hdr_len) && (strncasecmp((const char *)(hdr + j), "file", 4) == 0)) {
                    j += 4U;
                    if ((j < hdr_len) && (hdr[j] == (uint8_t)'"')) {
                        return true;
                    }
                }
            } else if ((j + 4U <= hdr_len) && (strncasecmp((const char *)(hdr + j), "file", 4) == 0)) {
                j += 4U;
                if ((j < hdr_len) &&
                    ((hdr[j] == (uint8_t)';') || (hdr[j] == (uint8_t)'\r') || (hdr[j] == (uint8_t)'\n'))) {
                    return true;
                }
            }
        }
    }
    return false;
}

static esp_err_t parse_multipart_boundary(httpd_req_t *req, char *out, size_t out_cap)
{
    char   ct[192];
    size_t i;
    const char *p;

    if ((out_cap < 4U) || (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK)) {
        return ESP_FAIL;
    }
    p = strstr(ct, "boundary=");
    if (p == NULL) {
        return ESP_FAIL;
    }
    p += 9U;
    if (*p == '"') {
        p++;
        for (i = 0U; (i + 1U < out_cap) && (*p != '\0') && (*p != '"'); i++) {
            out[i] = *p++;
        }
        out[i] = '\0';
    } else {
        i = 0U;
        while ((i + 1U < out_cap) && (*p != '\0') && (*p != ';') && !isspace((unsigned char)*p)) {
            out[i++] = *p++;
        }
        out[i] = '\0';
    }
    if ((out[0] == '\0') || (strlen(out) > 70U)) {
        return ESP_FAIL;
    }
    {
        char *sc = strchr(out, ';');
        if (sc != NULL) {
            *sc = '\0';
        }
    }
    while ((i = strlen(out)) > 0U && isspace((unsigned char)out[i - 1U])) {
        out[i - 1U] = '\0';
    }
    return ESP_OK;
}

static bool uri_query_display_on(httpd_req_t *req)
{
    char        qry[96];
    const char *p;

    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) {
        return false;
    }
    for (p = qry;;) {
        if (strncasecmp(p, "display=", 8) == 0) {
            const char *v = p + 8;

            if ((v[0] == '1') && ((v[1] == '\0') || (v[1] == '&'))) {
                return true;
            }
            if ((strncasecmp(v, "on", 2) == 0) && ((v[2] == '\0') || (v[2] == '&'))) {
                return true;
            }
            if ((strncasecmp(v, "yes", 3) == 0) && ((v[3] == '\0') || (v[3] == '&'))) {
                return true;
            }
        }
        while ((*p != '\0') && (*p != '&')) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        p++;
    }
    return false;
}

typedef struct {
    FILE        *fp;
    uint8_t      endmark[4U + BOUNDARY_MAX + 2U + ENDMARK_EXTRA];
    size_t       endmark_len;
    uint8_t      endmark2[3U + BOUNDARY_MAX + 2U + ENDMARK_EXTRA];
    size_t       endmark2_len;
    uint8_t      carry[4U + BOUNDARY_MAX + 2U + ENDMARK_EXTRA];
    size_t       carry_len;
    size_t       file_bytes;
    bool         found_end;
    size_t       max_file;
} bmp_stream_t;

static const uint8_t *find_mime_end(const uint8_t *buf, size_t len, const bmp_stream_t *s)
{
    const uint8_t *h;

    h = find_bytes(buf, len, s->endmark, s->endmark_len);
    if (h != NULL) {
        return h;
    }
    if (s->endmark2_len > 0U) {
        return find_bytes(buf, len, s->endmark2, s->endmark2_len);
    }
    return NULL;
}

static void bmp_stream_init(bmp_stream_t *s, FILE *fp, const char *boundary, size_t max_file)
{
    size_t bl;

    (void)memset(s, 0, sizeof(*s));
    s->fp       = fp;
    s->max_file = max_file;
    bl          = strlen(boundary);
    (void)memcpy(s->endmark, "\r\n--", 4U);
    (void)memcpy(s->endmark + 4U, boundary, bl);
    (void)memcpy(s->endmark + 4U + bl, "--", 2U);
    s->endmark_len = 4U + bl + 2U;
    (void)memcpy(s->endmark2, "\n--", 3U);
    (void)memcpy(s->endmark2 + 3U, boundary, bl);
    (void)memcpy(s->endmark2 + 3U + bl, "--", 2U);
    s->endmark2_len = 3U + bl + 2U;
}

static esp_err_t bmp_stream_feed(bmp_stream_t *s, const uint8_t *data, size_t len)
{
    uint8_t  work[MP_RECV_CHUNK + sizeof(s->carry)];
    size_t   wlen;
    size_t   keep;
    const uint8_t *hit;

    if ((s->found_end) || (len == 0U)) {
        return ESP_OK;
    }

    if (s->carry_len + len > sizeof(work)) {
        return ESP_FAIL;
    }
    (void)memcpy(work, s->carry, s->carry_len);
    (void)memcpy(work + s->carry_len, data, len);
    wlen = s->carry_len + len;

    hit = find_mime_end(work, wlen, s);
    if (hit != NULL) {
        size_t nbytes = (size_t)(hit - work);

        if (s->file_bytes + nbytes > s->max_file) {
            return ESP_ERR_NO_MEM;
        }
        if (nbytes > 0U) {
            if (fwrite(work, 1U, nbytes, s->fp) != nbytes) {
                return ESP_FAIL;
            }
            s->file_bytes += nbytes;
        }
        s->found_end = true;
        s->carry_len = 0U;
        return ESP_OK;
    }

    keep = (s->endmark_len > 1U) ? (s->endmark_len - 1U) : 1U;
    if (s->endmark2_len > 1U) {
        size_t k2 = s->endmark2_len - 1U;
        if (k2 > keep) {
            keep = k2;
        }
    }
    if (wlen > keep) {
        size_t safe = wlen - keep;

        if (s->file_bytes + safe > s->max_file) {
            return ESP_ERR_NO_MEM;
        }
        if (fwrite(work, 1U, safe, s->fp) != safe) {
            return ESP_FAIL;
        }
        s->file_bytes += safe;
        (void)memcpy(s->carry, work + safe, keep);
        s->carry_len = keep;
    } else {
        (void)memcpy(s->carry, work, wlen);
        s->carry_len = wlen;
    }
    return ESP_OK;
}

static esp_err_t bmp_stream_finish(bmp_stream_t *s)
{
    const uint8_t *hit;

    if (s->found_end) {
        return ESP_OK;
    }
    if (s->carry_len == 0U) {
        return ESP_FAIL;
    }
    hit = find_mime_end(s->carry, s->carry_len, s);
    if (hit == NULL) {
        return ESP_FAIL;
    }
    {
        size_t nbytes = (size_t)(hit - s->carry);

        if (s->file_bytes + nbytes > s->max_file) {
            return ESP_ERR_NO_MEM;
        }
        if (nbytes > 0U) {
            if (fwrite(s->carry, 1U, nbytes, s->fp) != nbytes) {
                return ESP_FAIL;
            }
            s->file_bytes += nbytes;
        }
    }
    s->found_end = true;
    s->carry_len = 0U;
    return ESP_OK;
}

/** 8.3：`W` + 7 位十六进制 + `.BMP`；若重名则换号重试。保存至 `/sdcard/picture/`。 */
static esp_err_t bmp_open_unique_upload_wb(FILE **out_fp, char *path, size_t path_max)
{
    static uint32_t s_salt;
    int             attempt;
    struct stat     st;
    const char     *gdir = lcd_gallery_dir_path();

    if (stat(gdir, &st) != 0) {
        (void)mkdir(gdir, 0755);
    }

    for (attempt = 0; attempt < 64; attempt++) {
        uint32_t tag = ((uint32_t)esp_timer_get_time() ^ (uint32_t)++s_salt ^ ((uint32_t)attempt * 0x9E3779B9U)) & 0x0FFFFFFFU;

        if (tag == 0U) {
            tag = (uint32_t)(attempt + 1U);
        }
        if (snprintf(path, path_max, "%s/W%07lX.BMP", gdir, (unsigned long)tag) >= (int)path_max) {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((stat(path, &st) == 0) && S_ISREG(st.st_mode)) {
            continue;
        }
        *out_fp = fopen(path, "wb");
        if (*out_fp != NULL) {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static esp_err_t bmp_upload_post_handler(httpd_req_t *req)
{
    char              boundary[BOUNDARY_MAX];
    char              path[WEB_BMP_PATH_MAX];
    uint8_t           stash[MP_STASH_MAX];
    size_t            stash_len = 0U;
    const uint8_t    *crlf2;
    size_t            body_off;
    size_t            body_len;
    size_t            recv_total = 0U;
    bmp_stream_t      bs;
    FILE             *fp = NULL;
    esp_err_t         err = ESP_FAIL;
    bool              want_display;
    status_t          pst;
    const char       *fail_why = "upload_fail";

    want_display = uri_query_display_on(req);

    if (sdcard_get_card() == NULL) {
        discard_post_remainder(req, 0U);
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_sd\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (req->content_len <= 0) {
        (void)httpd_resp_set_status(req, "411 Length Required");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_content_length\"}", HTTPD_RESP_USE_STRLEN);
    }

    if ((size_t)req->content_len > WEB_CTRL_BMP_UPLOAD_MAX_BYTES) {
        discard_post_remainder(req, 0U);
        (void)httpd_resp_set_status(req, "413 Payload Too Large");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"too_large\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (parse_multipart_boundary(req, boundary, sizeof(boundary)) != ESP_OK) {
        discard_post_remainder(req, 0U);
        (void)httpd_resp_set_status(req, "415 Unsupported Media Type");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_multipart_boundary\"}", HTTPD_RESP_USE_STRLEN);
    }

    while (stash_len < sizeof(stash)) {
        int rlen = httpd_req_recv(req, (char *)(stash + stash_len), sizeof(stash) - stash_len);
        if (rlen < 0) {
            discard_post_remainder(req, stash_len);
            (void)httpd_resp_set_status(req, "400 Bad Request");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"recv\"}", HTTPD_RESP_USE_STRLEN);
        }
        if (rlen == 0) {
            break;
        }
        stash_len += (size_t)rlen;
        crlf2 = find_header_body_separator(stash, stash_len);
        if (crlf2 != NULL) {
            break;
        }
    }

    crlf2 = find_header_body_separator(stash, stash_len);
    if (crlf2 == NULL) {
        discard_post_remainder(req, stash_len);
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_header_end\"}", HTTPD_RESP_USE_STRLEN);
    }

    body_off = (size_t)(crlf2 - stash) + header_body_sep_len(crlf2);
    {
        size_t prefix = 0U;
        size_t blen  = strlen(boundary);
        size_t olen  = 2U + blen + 2U;
        bool   line_ok;

        while ((prefix + 2U <= stash_len) && (stash[prefix] == (uint8_t)'\r') && (stash[prefix + 1U] == (uint8_t)'\n')) {
            prefix += 2U;
        }
        line_ok = false;
        if (prefix + 2U + blen + 2U <= stash_len) {
            if (memcmp(stash + prefix + 2U + blen, "\r\n", 2U) == 0) {
                line_ok = true;
            }
        }
        if (!line_ok && (prefix + 2U + blen + 1U <= stash_len) && (stash[prefix + 2U + blen] == (uint8_t)'\n')) {
            line_ok = true;
            olen    = 2U + blen + 1U;
        }
        if ((stash_len < prefix + olen) || (body_off < prefix + olen) || (memcmp(stash + prefix, "--", 2U) != 0) ||
            (memcmp(stash + prefix + 2U, boundary, blen) != 0) || !line_ok) {
            discard_post_remainder(req, stash_len);
            (void)httpd_resp_set_status(req, "400 Bad Request");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_multipart_preamble\"}", HTTPD_RESP_USE_STRLEN);
        }
    }

    if (!stash_headers_have_file_field(stash, body_off)) {
        discard_post_remainder(req, stash_len);
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_file_field\"}", HTTPD_RESP_USE_STRLEN);
    }

    recv_total = stash_len;

    err = bmp_open_unique_upload_wb(&fp, path, sizeof(path));
    if ((err != ESP_OK) || (fp == NULL)) {
        int         fe = errno;
        char        jerr[120];

        discard_post_remainder(req, recv_total);
        ESP_LOGW(TAG, "open upload dest failed err=%s errno=%d path=%s", esp_err_to_name(err), fe, path);
        (void)snprintf(jerr, sizeof(jerr), "{\"ok\":false,\"error\":\"open_fail\",\"errno\":%d}", fe);
        jerr[sizeof(jerr) - 1U] = '\0';
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, jerr, HTTPD_RESP_USE_STRLEN);
    }

    bmp_stream_init(&bs, fp, boundary, WEB_CTRL_BMP_UPLOAD_MAX_BYTES);

    body_len = stash_len - body_off;
    if (body_len > 0U) {
        size_t fed = 0U;

        /* stash 首段可大于 bmp_stream_feed 的 work 缓冲，需分块 */
        while (fed < body_len) {
            size_t chunk = body_len - fed;

            if (chunk > MP_RECV_CHUNK) {
                chunk = MP_RECV_CHUNK;
            }
            err = bmp_stream_feed(&bs, stash + body_off + fed, chunk);
            if (err != ESP_OK) {
                fail_why = (err == ESP_ERR_NO_MEM) ? "over_max" : "stream_feed";
                goto cleanup;
            }
            fed += chunk;
        }
    }

    while ((!bs.found_end) && (recv_total < (size_t)req->content_len)) {
        uint8_t chunk[MP_RECV_CHUNK];
        size_t  need = (size_t)req->content_len - recv_total;
        size_t  ask  = (need > sizeof(chunk)) ? sizeof(chunk) : need;
        int     rlen2 = httpd_req_recv(req, (char *)chunk, ask);

        if (rlen2 < 0) {
            err      = ESP_FAIL;
            fail_why = "recv_err";
            goto cleanup;
        }
        if (rlen2 == 0) {
            err      = ESP_FAIL;
            fail_why = "recv_short";
            goto cleanup;
        }
        recv_total += (size_t)rlen2;
        err = bmp_stream_feed(&bs, chunk, (size_t)rlen2);
        if (err != ESP_OK) {
            fail_why = (err == ESP_ERR_NO_MEM) ? "over_max" : "stream_feed";
            goto cleanup;
        }
    }

    if (!bs.found_end) {
        err = bmp_stream_finish(&bs);
        if (err != ESP_OK) {
            fail_why = "no_mime_end";
            goto cleanup;
        }
    }

    if (!bs.found_end || (bs.file_bytes < 54U)) {
        err      = ESP_FAIL;
        fail_why = !bs.found_end ? "truncated" : "small_file";
        goto cleanup;
    }

    while (recv_total < (size_t)req->content_len) {
        uint8_t sink[256];
        size_t  need = (size_t)req->content_len - recv_total;
        size_t  ask  = (need > sizeof(sink)) ? sizeof(sink) : need;
        int     rd   = httpd_req_recv(req, (char *)sink, ask);

        if (rd <= 0) {
            break;
        }
        recv_total += (size_t)rd;
    }

    (void)fclose(fp);
    fp = NULL;

    pst = lcd_gallery_probe_bmp(path);
    if (pst != STATUS_OK) {
        (void)unlink(path);
        discard_post_remainder(req, recv_total);
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_bmp\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* 图库列表仅在启动时扫盘；上传新文件后须重扫，否则按键切图仍用旧 s_count */
    lcd_gallery_rescan();

    {
        char json[384];
        char disp[48] = "skipped";

        if (want_display) {
            if (lcd_video_is_playing()) {
                lcd_video_request_stop();
            }
            if (lcd_gallery_post_show_path(path)) {
                (void)strncpy(disp, "queued", sizeof(disp) - 1U);
            } else {
                (void)strncpy(disp, "busy", sizeof(disp) - 1U);
            }
        }

        (void)snprintf(json, sizeof(json),
                       "{\"ok\":true,\"path\":\"%s\",\"bytes\":%u,\"display\":\"%s\"}", path,
                       (unsigned int)bs.file_bytes, disp);
        json[sizeof(json) - 1U] = '\0';
        (void)httpd_resp_set_status(req, "200 OK");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    }

cleanup:
    discard_post_remainder(req, recv_total);
    if (fp != NULL) {
        (void)fclose(fp);
    }
    (void)unlink(path);
    if (err == ESP_ERR_NO_MEM) {
        (void)httpd_resp_set_status(req, "413 Payload Too Large");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"file_too_large\"}", HTTPD_RESP_USE_STRLEN);
    }
    {
        char jbuf[160];
        (void)snprintf(jbuf, sizeof(jbuf), "{\"ok\":false,\"error\":\"upload_fail\",\"reason\":\"%s\"}", fail_why);
        jbuf[sizeof(jbuf) - 1U] = '\0';
        ESP_LOGW(TAG, "upload failed: %s (err=%s file_bytes=%u)", fail_why, esp_err_to_name(err),
                 (unsigned int)bs.file_bytes);
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, jbuf, HTTPD_RESP_USE_STRLEN);
    }
}

/* ---------- SD 图库：列表 / 预览 / 删除 ---------- */

typedef struct {
    char           name[WEB_GALLERY_NAME_MAX + 1U];
    unsigned long  size_bytes;
    bool           is_bmp;
} web_gallery_entry_t;

static bool web_path_suffix_icase(const char *path, const char *suf)
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

/** 仅允许 `picture/` 下单层文件名：字母数字与 `._-~`（FAT 8.3 短名），且后缀为 `.bmp` / `.bin`。 */
static bool web_gallery_basename_ok(const char *name)
{
    size_t i;
    size_t len;

    if (name == NULL) {
        return false;
    }
    len = strlen(name);
    if ((len == 0U) || (len > WEB_GALLERY_NAME_MAX) || (name[0] == '.')) {
        return false;
    }
    for (i = 0U; i < len; i++) {
        const unsigned char c = (unsigned char)name[i];

        if ((isalnum(c) != 0) || (c == '.') || (c == '_') || (c == '-') || (c == '~')) {
            continue;
        }
        return false;
    }
    if (!web_path_suffix_icase(name, ".bmp") && !web_path_suffix_icase(name, ".bin")) {
        return false;
    }
    return true;
}

/** 去掉 `picture/` 或路径前缀，只保留 basename。 */
static void web_gallery_normalize_name(const char *in, char *out, size_t out_cap)
{
    const char *base;
    const char *slash;

    if ((out == NULL) || (out_cap == 0U)) {
        return;
    }
    out[0] = '\0';
    if (in == NULL) {
        return;
    }
    base = in;
    if (strncasecmp(in, "picture/", 8) == 0) {
        base = in + 8;
    } else if (strncasecmp(in, "/sdcard/picture/", 16) == 0) {
        base = in + 16;
    } else {
        slash = strrchr(in, '/');
        if (slash != NULL) {
            base = slash + 1;
        }
    }
    (void)snprintf(out, out_cap, "%.*s", (int)WEB_GALLERY_NAME_MAX, base);
}

static void web_gallery_sort_entries(web_gallery_entry_t *ents, size_t n)
{
    size_t i;
    size_t j;

    for (i = 0U; i + 1U < n; i++) {
        for (j = i + 1U; j < n; j++) {
            if (strcmp(ents[i].name, ents[j].name) > 0) {
                web_gallery_entry_t tmp = ents[i];

                ents[i] = ents[j];
                ents[j] = tmp;
            }
        }
    }
}

static esp_err_t gallery_list_get_handler(httpd_req_t *req)
{
    DIR              *dir;
    struct dirent    *ent;
    web_gallery_entry_t ents[WEB_GALLERY_MAX_FILES];
    size_t            nents = 0U;
    static char       json[WEB_GALLERY_JSON_MAX];
    size_t            pos = 0U;
    int               w;
    size_t            k;

    if (sdcard_get_card() == NULL) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_sd\"}", HTTPD_RESP_USE_STRLEN);
    }

    dir = opendir(lcd_gallery_dir_path());
    if (dir == NULL) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"opendir\"}", HTTPD_RESP_USE_STRLEN);
    }

    while ((ent = readdir(dir)) != NULL) {
        struct stat st;
        char        full[WEB_BMP_PATH_MAX];
        const char *gdir = lcd_gallery_dir_path();

        if ((ent->d_name[0] == '\0') || (strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
            continue;
        }
        if (!web_path_suffix_icase(ent->d_name, ".bin") && !web_path_suffix_icase(ent->d_name, ".bmp")) {
            continue;
        }
        if (nents >= WEB_GALLERY_MAX_FILES) {
            break;
        }
        if (snprintf(full, sizeof(full), "%s/%s", gdir, ent->d_name) >= (int)sizeof(full)) {
            continue;
        }
        if (stat(full, &st) != 0) {
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        (void)memset(&ents[nents], 0, sizeof(ents[nents]));
        (void)snprintf(ents[nents].name, sizeof(ents[nents].name), "%.*s", (int)WEB_GALLERY_NAME_MAX, ent->d_name);
        ents[nents].size_bytes                 = (unsigned long)st.st_size;
        ents[nents].is_bmp                     = web_path_suffix_icase(ent->d_name, ".bmp");
        nents++;
    }
    (void)closedir(dir);

    web_gallery_sort_entries(ents, nents);

    w = snprintf(json + pos, sizeof(json) - pos, "{\"ok\":true,\"files\":[");
    if ((w <= 0) || ((size_t)w >= sizeof(json) - pos)) {
        goto overflow;
    }
    pos += (size_t)w;

    for (k = 0U; k < nents; k++) {
        const char *kind = ents[k].is_bmp ? "bmp" : "bin";

        if (k > 0U) {
            if (pos + 2U >= sizeof(json)) {
                goto overflow;
            }
            json[pos++] = ',';
        }
        w = snprintf(json + pos, sizeof(json) - pos,
                       "{\"name\":\"%s\",\"bytes\":%lu,\"kind\":\"%s\"}", ents[k].name, ents[k].size_bytes, kind);
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

static esp_err_t gallery_bmp_get_handler(httpd_req_t *req)
{
    char         qry[192];
    char         name[WEB_GALLERY_NAME_MAX + 1U];
    char         path[WEB_BMP_PATH_MAX];
    struct stat  st;
    FILE        *fp = NULL;
    esp_err_t    er;

    if (sdcard_get_card() == NULL) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "no_sd", HTTPD_RESP_USE_STRLEN);
    }

    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_name\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (httpd_query_key_value(qry, "name", name, sizeof(name)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_name\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (!web_gallery_basename_ok(name) || !web_path_suffix_icase(name, ".bmp")) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_name\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (snprintf(path, sizeof(path), "%s/%s", lcd_gallery_dir_path(), name) >= (int)sizeof(path)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"path\"}", HTTPD_RESP_USE_STRLEN);
    }

    if ((stat(path, &st) != 0) || !S_ISREG(st.st_mode)) {
        (void)httpd_resp_set_status(req, "404 Not Found");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"not_found\"}", HTTPD_RESP_USE_STRLEN);
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"open\"}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "image/bmp");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=120");

    for (;;) {
        uint8_t buf[GALLERY_BMP_SEND_CHUNK];
        size_t  n = fread(buf, 1U, sizeof(buf), fp);

        if (n == 0U) {
            break;
        }
        er = httpd_resp_send_chunk(req, (const char *)buf, n);
        if (er != ESP_OK) {
            (void)fclose(fp);
            return er;
        }
    }
    (void)fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t gallery_delete_post_handler(httpd_req_t *req)
{
    char       qry[192];
    char       name[WEB_GALLERY_NAME_MAX + 1U];
    char       path[WEB_BMP_PATH_MAX];
    char       jbuf[192];
    int        ur;

    if (req->content_len > 0) {
        discard_post_remainder(req, 0U);
    }

    if (sdcard_get_card() == NULL) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_sd\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_name\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (httpd_query_key_value(qry, "name", name, sizeof(name)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_name\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (!web_gallery_basename_ok(name)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_name\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (snprintf(path, sizeof(path), "%s/%s", lcd_gallery_dir_path(), name) >= (int)sizeof(path)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"path\"}", HTTPD_RESP_USE_STRLEN);
    }

    ur = unlink(path);
    if (ur != 0) {
        (void)snprintf(jbuf, sizeof(jbuf), "{\"ok\":false,\"error\":\"unlink\",\"errno\":%d}", errno);
        jbuf[sizeof(jbuf) - 1U] = '\0';
        (void)httpd_resp_set_status(req, "404 Not Found");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, jbuf, HTTPD_RESP_USE_STRLEN);
    }

    lcd_gallery_rescan();

    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t gallery_prefs_get_handler(httpd_req_t *req)
{
    char        boot[NVS_LCD_GAL_BOOT_SIZE];
    static char json[192];
    int         w;

    (void)memset(boot, 0, sizeof(boot));
    (void)nvs_lcd_gallery_boot_name_get(boot, sizeof(boot));
    if (!web_gallery_basename_ok(boot)) {
        boot[0] = '\0';
    }

    w = snprintf(json, sizeof(json), "{\"ok\":true,\"boot_name\":\"%s\"}", boot);
    if ((w <= 0) || ((size_t)w >= sizeof(json))) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"json\"}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t gallery_prefs_post_handler(httpd_req_t *req)
{
    char       qry[256];
    char       boot[NVS_LCD_GAL_BOOT_SIZE];
    char       path[WEB_BMP_PATH_MAX];
    struct stat st;

    if (req->content_len > 0) {
        discard_post_remainder(req, 0U);
    }

    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_query\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (httpd_query_key_value(qry, "boot_name", boot, sizeof(boot)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_boot_name\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (boot[0] == '\0') {
        if (!nvs_lcd_gallery_boot_name_set(NULL)) {
            (void)httpd_resp_set_status(req, "500 Internal Server Error");
            (void)httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"nvs\"}", HTTPD_RESP_USE_STRLEN);
        }
        (void)httpd_resp_set_status(req, "200 OK");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true,\"boot_name\":\"\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!web_gallery_basename_ok(boot)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_name\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (sdcard_get_card() == NULL) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_sd\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (snprintf(path, sizeof(path), "%s/%s", lcd_gallery_dir_path(), boot) >= (int)sizeof(path)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"path\"}", HTTPD_RESP_USE_STRLEN);
    }
    if ((stat(path, &st) != 0) || !S_ISREG(st.st_mode)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"not_found\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (!nvs_lcd_gallery_boot_name_set(boot)) {
        (void)httpd_resp_set_status(req, "500 Internal Server Error");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"nvs\"}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    {
        static char json[192];
        int         w = snprintf(json, sizeof(json), "{\"ok\":true,\"boot_name\":\"%s\"}", boot);

        if ((w <= 0) || ((size_t)w >= sizeof(json))) {
            return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
        }
        return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    }
}

static esp_err_t gallery_show_post_handler(httpd_req_t *req)
{
    char        qry[192];
    char        raw[WEB_GALLERY_NAME_MAX + 1U];
    char        name[WEB_GALLERY_NAME_MAX + 1U];
    char        path[WEB_BMP_PATH_MAX];
    struct stat st;

    if (req->content_len > 0) {
        discard_post_remainder(req, 0U);
    }

    if (sdcard_get_card() == NULL) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_sd\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_name\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (httpd_query_key_value(qry, "name", raw, sizeof(raw)) != ESP_OK) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"need_name\"}", HTTPD_RESP_USE_STRLEN);
    }
    web_gallery_normalize_name(raw, name, sizeof(name));
    if (!web_gallery_basename_ok(name)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_name\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (snprintf(path, sizeof(path), "%s/%s", lcd_gallery_dir_path(), name) >= (int)sizeof(path)) {
        (void)httpd_resp_set_status(req, "400 Bad Request");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"path\"}", HTTPD_RESP_USE_STRLEN);
    }
    if ((stat(path, &st) != 0) || !S_ISREG(st.st_mode)) {
        (void)httpd_resp_set_status(req, "404 Not Found");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"not_found\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (lcd_video_is_playing()) {
        lcd_video_request_stop();
    }
    if (!lcd_gallery_post_show_name(name)) {
        (void)httpd_resp_set_status(req, "503 Service Unavailable");
        (void)httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"busy\"}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"display\":\"queued\"}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_bmp_upload_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/upload/bmp", .method = HTTP_POST, .handler = bmp_upload_post_handler, .user_ctx = NULL},
        {.uri = "/api/gallery/list", .method = HTTP_GET, .handler = gallery_list_get_handler, .user_ctx = NULL},
        {.uri = "/api/gallery/bmp", .method = HTTP_GET, .handler = gallery_bmp_get_handler, .user_ctx = NULL},
        {.uri = "/api/gallery/delete", .method = HTTP_POST, .handler = gallery_delete_post_handler, .user_ctx = NULL},
        {.uri = "/api/gallery/prefs", .method = HTTP_GET, .handler = gallery_prefs_get_handler, .user_ctx = NULL},
        {.uri = "/api/gallery/prefs", .method = HTTP_POST, .handler = gallery_prefs_post_handler, .user_ctx = NULL},
        {.uri = "/api/gallery/show", .method = HTTP_POST, .handler = gallery_show_post_handler, .user_ctx = NULL},
    };
    esp_err_t err;
    size_t    i;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (i = 0U; i < (sizeof(uris) / sizeof(uris[0])); i++) {
        err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
