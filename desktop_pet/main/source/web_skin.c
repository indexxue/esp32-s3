/**
 * @file web_skin.c
 * @brief Upload a pet/ zip to staging, reboot, then swap over /sdcard/pet/.
 */

#include "web_skin.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"

#include "board.h"
#include "sdcard.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEB_SKIN_PATH_MAX     (200U)
#define WEB_SKIN_REL_MAX      (160U)
#define WEB_SKIN_ZIP_MAX      (4U * 1024U * 1024U)
#define WEB_SKIN_FILE_MAX     (512U * 1024U)
#define WEB_SKIN_MAX_ENTRIES  (64U)
#define WEB_SKIN_RECV_CHUNK   (1024U)
#define WEB_SKIN_COPY_CHUNK   (1024U)
#define WEB_SKIN_RMTREE_MAX   (8U)
#define WEB_SKIN_REBOOT_MS    (800U)
/* 8.3-safe names: this project's FatFS default is LFN_NONE until sdkconfig.defaults enables LFN. */
#define WEB_SKIN_ZIP_REL      "skinup.zip"
#define WEB_SKIN_NEXT_REL     "petnext"
#define WEB_SKIN_LIVE_REL     "pet"

static const char *TAG = "web_skin";

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool path_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t na;
    size_t nb;

    if ((out == NULL) || (a == NULL) || (b == NULL) || (cap < 4U) || (b[0] == '\0')) {
        return false;
    }
    na = strlen(a);
    nb = strlen(b);
    if ((na + 1U + nb + 1U) > cap) {
        return false;
    }
    (void)memcpy(out, a, na);
    out[na] = '/';
    (void)memcpy(out + na + 1U, b, nb);
    out[na + 1U + nb] = '\0';
    return true;
}

static void *skin_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (p == NULL) {
        p = malloc(n);
    }
    return p;
}

static bool rel_ok(const char *rel)
{
    size_t i;
    size_t len;
    size_t seg = 0U;

    if ((rel == NULL) || (rel[0] == '\0') || (rel[0] == '/')) {
        return false;
    }
    len = strlen(rel);
    if (len > WEB_SKIN_REL_MAX) {
        return false;
    }
    for (i = 0U; i < len; i++) {
        const unsigned char c = (unsigned char)rel[i];

        if (c == '/') {
            if ((seg == 0U) || ((seg == 2U) && (rel[i - 2U] == '.') && (rel[i - 1U] == '.'))) {
                return false;
            }
            seg = 0U;
            continue;
        }
        if ((c == '.') || (c == '_') || (c == '-') ||
            ((c >= '0') && (c <= '9')) ||
            ((c >= 'A') && (c <= 'Z')) ||
            ((c >= 'a') && (c <= 'z'))) {
            seg++;
            continue;
        }
        return false;
    }
    return (seg > 0U) && !((seg == 2U) && (rel[len - 2U] == '.') && (rel[len - 1U] == '.'));
}

static void strip_pet_prefix(char *name)
{
    size_t n;

    while ((strncmp(name, "./", 2) == 0)) {
        n = strlen(name + 2U);
        (void)memmove(name, name + 2U, n + 1U);
    }
    if (strncmp(name, "pet/", 4) == 0) {
        n = strlen(name + 4U);
        (void)memmove(name, name + 4U, n + 1U);
    }
}

static bool name_skip(const char *rel)
{
    if ((rel == NULL) || (rel[0] == '\0')) {
        return true;
    }
    if ((strncmp(rel, "__MACOSX", 8) == 0) || (strstr(rel, ".DS_Store") != NULL)) {
        return true;
    }
    if ((strcmp(rel, "config") == 0) || (strncmp(rel, "config/", 7) == 0)) {
        return true;
    }
    if ((strcmp(rel, "record") == 0) || (strncmp(rel, "record/", 7) == 0)) {
        return true;
    }
    return false;
}

static void rmtree(const char *abs, uint8_t depth)
{
    DIR *dir;
    struct dirent *ent;
    char child[WEB_SKIN_PATH_MAX];

    if ((abs == NULL) || (abs[0] == '\0') || (depth > WEB_SKIN_RMTREE_MAX)) {
        return;
    }
    dir = opendir(abs);
    if (dir == NULL) {
        (void)unlink(abs);
        (void)rmdir(abs);
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
            continue;
        }
        if (!path_join(child, sizeof(child), abs, ent->d_name)) {
            continue;
        }
        rmtree(child, (uint8_t)(depth + 1U));
    }
    (void)closedir(dir);
    (void)rmdir(abs);
}

static bool mkdir_p_parent(const char *abs)
{
    char tmp[WEB_SKIN_PATH_MAX];
    size_t n;
    size_t i;

    n = strlen(abs);
    if ((n == 0U) || (n >= sizeof(tmp))) {
        return false;
    }
    (void)memcpy(tmp, abs, n + 1U);
    for (i = 1U; i < n; i++) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        (void)mkdir(tmp, 0775);
        tmp[i] = '/';
    }
    return true;
}

static bool copy_store(FILE *in, FILE *out, uint32_t len)
{
    uint8_t buf[WEB_SKIN_COPY_CHUNK];

    while (len > 0U) {
        size_t n = sizeof(buf);
        size_t got;

        if (n > len) {
            n = len;
        }
        got = fread(buf, 1U, n, in);
        if (got != n) {
            return false;
        }
        if (fwrite(buf, 1U, n, out) != n) {
            return false;
        }
        len -= (uint32_t)n;
    }
    return true;
}

static bool inflate_store(FILE *in, uint32_t comp_len, FILE *out, uint32_t uncomp_len)
{
    tinfl_decompressor *dec = NULL;
    uint8_t *comp = NULL;
    uint8_t *raw = NULL;
    size_t in_sz;
    size_t out_sz;
    tinfl_status st;
    bool ok = false;

    if ((comp_len == 0U) || (uncomp_len == 0U) ||
        (comp_len > WEB_SKIN_FILE_MAX) || (uncomp_len > WEB_SKIN_FILE_MAX)) {
        return false;
    }
    dec = (tinfl_decompressor *)skin_alloc(sizeof(*dec));
    comp = (uint8_t *)skin_alloc(comp_len);
    raw = (uint8_t *)skin_alloc(uncomp_len);
    if ((dec == NULL) || (comp == NULL) || (raw == NULL)) {
        goto done;
    }
    if (fread(comp, 1U, comp_len, in) != comp_len) {
        goto done;
    }
    tinfl_init(dec);
    in_sz = comp_len;
    out_sz = uncomp_len;
    st = tinfl_decompress(dec, comp, &in_sz, raw, raw, &out_sz,
                          TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if ((st != TINFL_STATUS_DONE) || (out_sz != (size_t)uncomp_len)) {
        goto done;
    }
    if (fwrite(raw, 1U, uncomp_len, out) != uncomp_len) {
        goto done;
    }
    ok = true;

done:
    free(dec);
    free(comp);
    free(raw);
    return ok;
}

static bool skip_bytes(FILE *in, uint32_t len)
{
    return fseek(in, (long)len, SEEK_CUR) == 0;
}

static bool extract_one(FILE *zf, const char *staging, char *name, uint16_t method,
                        uint32_t comp, uint32_t uncomp, uint16_t flags, bool *saw_pack)
{
    char abs[WEB_SKIN_PATH_MAX];
    FILE *out;
    bool ok;

    if ((flags & 0x0008U) != 0U) {
        return false;
    }
    strip_pet_prefix(name);
    if (name_skip(name) || (name[0] == '\0')) {
        return skip_bytes(zf, comp);
    }
    if (name[strlen(name) - 1U] == '/') {
        return skip_bytes(zf, comp);
    }
    if (!rel_ok(name)) {
        return false;
    }
    if (!path_join(abs, sizeof(abs), staging, name)) {
        return false;
    }
    if ((uncomp == 0U) && (comp == 0U)) {
        return true;
    }
    if ((uncomp > WEB_SKIN_FILE_MAX) || (comp > WEB_SKIN_FILE_MAX)) {
        return false;
    }
    if (!mkdir_p_parent(abs)) {
        return false;
    }
    out = fopen(abs, "wb");
    if (out == NULL) {
        return false;
    }
    if (method == 0U) {
        ok = copy_store(zf, out, comp);
    } else if (method == 8U) {
        ok = inflate_store(zf, comp, out, uncomp);
    } else {
        ok = false;
    }
    (void)fclose(out);
    if (!ok) {
        (void)unlink(abs);
        return false;
    }
    if ((saw_pack != NULL) && (strcmp(name, "pack.bin") == 0)) {
        *saw_pack = true;
    }
    return true;
}

static bool unzip_to_staging(const char *zip_abs, const char *staging)
{
    FILE *zf;
    uint8_t hdr[30];
    char name[WEB_SKIN_REL_MAX + 4U];
    uint32_t nfile = 0U;
    bool saw_pack = false;

    zf = fopen(zip_abs, "rb");
    if (zf == NULL) {
        return false;
    }
    (void)mkdir(staging, 0775);

    for (;;) {
        uint32_t sig;
        uint16_t flags;
        uint16_t method;
        uint16_t nlen;
        uint16_t elen;
        uint32_t comp;
        uint32_t uncomp;

        if (fread(hdr, 1U, 30U, zf) != 30U) {
            break;
        }
        sig = rd32(hdr);
        if ((sig == 0x02014b50UL) || (sig == 0x06054b50UL)) {
            break;
        }
        if (sig != 0x04034b50UL) {
            (void)fclose(zf);
            return false;
        }
        flags = rd16(hdr + 6U);
        method = rd16(hdr + 8U);
        comp = rd32(hdr + 18U);
        uncomp = rd32(hdr + 22U);
        nlen = rd16(hdr + 26U);
        elen = rd16(hdr + 28U);
        if ((nlen == 0U) || (nlen > WEB_SKIN_REL_MAX)) {
            (void)fclose(zf);
            return false;
        }
        if (fread(name, 1U, nlen, zf) != nlen) {
            (void)fclose(zf);
            return false;
        }
        name[nlen] = '\0';
        if (!skip_bytes(zf, elen)) {
            (void)fclose(zf);
            return false;
        }
        nfile++;
        if (nfile > WEB_SKIN_MAX_ENTRIES) {
            (void)fclose(zf);
            return false;
        }
        if (!extract_one(zf, staging, name, method, comp, uncomp, flags, &saw_pack)) {
            (void)fclose(zf);
            return false;
        }
    }
    (void)fclose(zf);
    return saw_pack;
}

static void skin_abs(char *out, size_t cap, const char *rel)
{
    (void)path_join(out, cap, BOARD_SDCARD_MOUNT_POINT, rel);
}

void web_skin_commit_pending(void)
{
    char staging[WEB_SKIN_PATH_MAX];
    char live[WEB_SKIN_PATH_MAX];
    char pack[WEB_SKIN_PATH_MAX];
    struct stat st;

    if (sdcard_get_card() == NULL) {
        return;
    }
    skin_abs(staging, sizeof(staging), WEB_SKIN_NEXT_REL);
    skin_abs(live, sizeof(live), WEB_SKIN_LIVE_REL);
    if (!path_join(pack, sizeof(pack), staging, "pack.bin")) {
        return;
    }
    if ((stat(pack, &st) != 0) || !S_ISREG(st.st_mode)) {
        return;
    }
    ESP_LOGI(TAG, "commit pending skin -> %s", live);
    rmtree(live, 0U);
    if (rename(staging, live) != 0) {
        ESP_LOGE(TAG, "rename staging failed");
        return;
    }
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WEB_SKIN_REBOOT_MS));
    esp_restart();
}

static void schedule_reboot(void)
{
    if (xTaskCreate(reboot_task, "skin_rb", 2048, NULL, 5, NULL) != pdPASS) {
        esp_restart();
    }
}

static void json_err(httpd_req_t *req, const char *status, const char *err)
{
    char buf[96];
    int n;

    n = snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", err);
    (void)httpd_resp_set_status(req, status);
    (void)httpd_resp_set_type(req, "application/json");
    if ((n > 0) && ((size_t)n < sizeof(buf))) {
        (void)httpd_resp_send(req, buf, (size_t)n);
    } else {
        (void)httpd_resp_send(req, "{\"ok\":false,\"error\":\"err\"}", HTTPD_RESP_USE_STRLEN);
    }
}

static void discard_rest(httpd_req_t *req, size_t got)
{
    size_t cl = (req->content_len > 0) ? (size_t)req->content_len : 0U;

    while (got < cl) {
        char b[256];
        size_t ask = cl - got;
        int n;

        if (ask > sizeof(b)) {
            ask = sizeof(b);
        }
        n = httpd_req_recv(req, b, ask);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
}

static esp_err_t skin_post_handler(httpd_req_t *req)
{
    char zip_abs[WEB_SKIN_PATH_MAX];
    char staging[WEB_SKIN_PATH_MAX];
    FILE *fp;
    size_t total;
    size_t got = 0U;
    uint8_t magic[4];
    bool magic_ok = false;

    zip_abs[0] = '\0';
    staging[0] = '\0';

    if (sdcard_get_card() == NULL) {
        discard_rest(req, 0U);
        json_err(req, "503 Service Unavailable", "no_sd");
        return ESP_OK;
    }
    if (req->content_len <= 4) {
        json_err(req, "411 Length Required", "need_zip");
        return ESP_OK;
    }
    total = (size_t)req->content_len;
    if (total > WEB_SKIN_ZIP_MAX) {
        discard_rest(req, 0U);
        json_err(req, "413 Payload Too Large", "too_large");
        return ESP_OK;
    }

    if (!path_join(zip_abs, sizeof(zip_abs), BOARD_SDCARD_MOUNT_POINT, WEB_SKIN_ZIP_REL) ||
        !path_join(staging, sizeof(staging), BOARD_SDCARD_MOUNT_POINT, WEB_SKIN_NEXT_REL)) {
        discard_rest(req, 0U);
        json_err(req, "500 Internal Server Error", "path");
        return ESP_OK;
    }
    rmtree(staging, 0U);
    (void)unlink(zip_abs);

    fp = fopen(zip_abs, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "fopen %s failed errno=%d", zip_abs, errno);
        discard_rest(req, 0U);
        json_err(req, "500 Internal Server Error", "open_zip");
        return ESP_OK;
    }

    while (got < total) {
        char buf[WEB_SKIN_RECV_CHUNK];
        size_t ask = total - got;
        int n;

        if (ask > sizeof(buf)) {
            ask = sizeof(buf);
        }
        n = httpd_req_recv(req, buf, ask);
        if (n <= 0) {
            (void)fclose(fp);
            (void)unlink(zip_abs);
            json_err(req, "400 Bad Request", "recv");
            return ESP_OK;
        }
        if ((got == 0U) && (n >= 4)) {
            (void)memcpy(magic, buf, 4U);
            magic_ok = (rd32(magic) == 0x04034b50UL);
        }
        if (fwrite(buf, 1U, (size_t)n, fp) != (size_t)n) {
            (void)fclose(fp);
            (void)unlink(zip_abs);
            discard_rest(req, got + (size_t)n);
            json_err(req, "500 Internal Server Error", "write");
            return ESP_OK;
        }
        got += (size_t)n;
    }
    (void)fclose(fp);

    if (!magic_ok) {
        (void)unlink(zip_abs);
        json_err(req, "415 Unsupported Media Type", "not_zip");
        return ESP_OK;
    }

    if (!unzip_to_staging(zip_abs, staging)) {
        rmtree(staging, 0U);
        (void)unlink(zip_abs);
        json_err(req, "400 Bad Request", "bad_zip");
        return ESP_OK;
    }
    (void)unlink(zip_abs);

    ESP_LOGI(TAG, "skin staged, reboot");
    (void)httpd_resp_set_status(req, "200 OK");
    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_send(req, "{\"ok\":true,\"reboot\":true}", HTTPD_RESP_USE_STRLEN);
    schedule_reboot();
    return ESP_OK;
}

esp_err_t web_skin_http_register(httpd_handle_t server)
{
    const httpd_uri_t uri = {
        .uri = "/api/pet/skin",
        .method = HTTP_POST,
        .handler = skin_post_handler,
        .user_ctx = NULL,
    };
    esp_err_t err;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/pet/skin failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "skin upload API registered");
    return ESP_OK;
}
