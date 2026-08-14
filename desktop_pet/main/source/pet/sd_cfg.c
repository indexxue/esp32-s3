/**
 * @file sd_cfg.c
 * @brief Load /sdcard/config (KEY=VALUE) and scan declared roots.
 */

#include "sd_cfg.h"

#if defined(ESP_PLATFORM)
#include "log.h"
#define SD_CFG_INFO(...) LOG_INFO(__VA_ARGS__)
#define SD_CFG_WARN(...) LOG_WARN(__VA_ARGS__)
#else
#define SD_CFG_INFO(...) ((void)0)
#define SD_CFG_WARN(...) ((void)0)
#endif

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#else
#include <windows.h>
#endif

#define SD_CFG_SCAN_DEPTH (2U)

typedef struct {
    char key[SD_CFG_KEY_LEN];
    char val[SD_CFG_VAL_LEN];
} sd_cfg_kv_t;

typedef struct {
    char rel[SD_CFG_REL_LEN];
    uint32_t size;
} sd_cfg_file_t;

static char s_mount[SD_CFG_PATH_MAX];
static char s_content_path[SD_CFG_PATH_MAX];
static char s_record_path[SD_CFG_PATH_MAX];
static sd_cfg_kv_t s_keys[SD_CFG_MAX_KEYS];
static uint8_t s_key_n;
static sd_cfg_file_t s_files[SD_CFG_MAX_FILES];
static uint8_t s_file_n;
static bool s_loaded;

static void str_trim(char *s)
{
    size_t n;
    char *p = s;

    while ((*p == ' ') || (*p == '\t') || (*p == '\r')) {
        p++;
    }
    if (p != s) {
        (void)memmove(s, p, strlen(p) + 1U);
    }
    n = strlen(s);
    while ((n > 0U) && ((s[n - 1U] == ' ') || (s[n - 1U] == '\t') ||
                         (s[n - 1U] == '\r') || (s[n - 1U] == '\n'))) {
        n--;
        s[n] = '\0';
    }
}

static bool key_ok(const char *k)
{
    if ((k == NULL) || (k[0] == '\0')) {
        return false;
    }
    for (; *k != '\0'; k++) {
        if (!(isalnum((unsigned char)*k) || (*k == '_'))) {
            return false;
        }
    }
    return true;
}

static bool rel_ok(const char *rel)
{
    if ((rel == NULL) || (rel[0] == '\0')) {
        return false;
    }
    if ((rel[0] == '/') || (rel[0] == '\\') || (strstr(rel, "..") != NULL)) {
        return false;
    }
    return true;
}

static bool str_copy(char *dst, size_t cap, const char *src)
{
    size_t n;

    if ((dst == NULL) || (src == NULL) || (cap < 2U)) {
        return false;
    }
    n = strlen(src);
    if (n >= cap) {
        return false;
    }
    (void)memcpy(dst, src, n + 1U);
    return true;
}

static void kv_set(const char *key, const char *val)
{
    uint8_t i;

    if (!key_ok(key) || (val == NULL)) {
        return;
    }
    for (i = 0; i < s_key_n; i++) {
        if (strcmp(s_keys[i].key, key) == 0) {
            (void)str_copy(s_keys[i].val, sizeof(s_keys[i].val), val);
            return;
        }
    }
    if (s_key_n >= SD_CFG_MAX_KEYS) {
        return;
    }
    if (!str_copy(s_keys[s_key_n].key, sizeof(s_keys[s_key_n].key), key) ||
        !str_copy(s_keys[s_key_n].val, sizeof(s_keys[s_key_n].val), val)) {
        return;
    }
    s_key_n++;
}

static bool join2(char *out, size_t cap, const char *a, const char *b)
{
    size_t na;
    size_t nb;

    if ((out == NULL) || (cap < 4U) || (a == NULL) || (b == NULL) || (b[0] == '\0')) {
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

static void add_file(const char *rel, uint32_t size)
{
    if ((rel == NULL) || (rel[0] == '\0') || (s_file_n >= SD_CFG_MAX_FILES)) {
        return;
    }
    if (!str_copy(s_files[s_file_n].rel, sizeof(s_files[s_file_n].rel), rel)) {
        return;
    }
    s_files[s_file_n].size = size;
    s_file_n++;
}

static void scan_tree(const char *abs, const char *rel, uint8_t depth_left);

#ifdef _WIN32
static void scan_tree(const char *abs, const char *rel, uint8_t depth_left)
{
    char pattern[SD_CFG_PATH_MAX];
    char child_abs[SD_CFG_PATH_MAX];
    char child_rel[SD_CFG_REL_LEN];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    size_t na;

    na = strlen(abs);
    if ((na + 3U) > sizeof(pattern)) {
        return;
    }
    (void)memcpy(pattern, abs, na);
    pattern[na] = '\\';
    pattern[na + 1U] = '*';
    pattern[na + 2U] = '\0';
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((fd.cFileName[0] == '.') || (strcmp(fd.cFileName, SD_CFG_NAME) == 0)) {
            continue;
        }
        if (!join2(child_abs, sizeof(child_abs), abs, fd.cFileName)) {
            continue;
        }
        if ((rel != NULL) && (rel[0] != '\0')) {
            if (!join2(child_rel, sizeof(child_rel), rel, fd.cFileName)) {
                continue;
            }
        } else if (!str_copy(child_rel, sizeof(child_rel), fd.cFileName)) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth_left > 0U) {
                scan_tree(child_abs, child_rel, (uint8_t)(depth_left - 1U));
            }
        } else {
            add_file(child_rel, (uint32_t)fd.nFileSizeLow);
        }
    } while (FindNextFileA(h, &fd) != 0);
    (void)FindClose(h);
}
#else
static void scan_tree(const char *abs, const char *rel, uint8_t depth_left)
{
    DIR *dir;
    struct dirent *ent;
    struct stat st;
    char child_abs[SD_CFG_PATH_MAX];
    char child_rel[SD_CFG_REL_LEN];

    dir = opendir(abs);
    if (dir == NULL) {
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        if ((ent->d_name[0] == '.') || (strcmp(ent->d_name, SD_CFG_NAME) == 0)) {
            continue;
        }
        if (!join2(child_abs, sizeof(child_abs), abs, ent->d_name)) {
            continue;
        }
        if ((rel != NULL) && (rel[0] != '\0')) {
            if (!join2(child_rel, sizeof(child_rel), rel, ent->d_name)) {
                continue;
            }
        } else if (!str_copy(child_rel, sizeof(child_rel), ent->d_name)) {
            continue;
        }
        if (stat(child_abs, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth_left > 0U) {
                scan_tree(child_abs, child_rel, (uint8_t)(depth_left - 1U));
            }
        } else if (S_ISREG(st.st_mode)) {
            add_file(child_rel, (uint32_t)st.st_size);
        }
    }
    (void)closedir(dir);
}
#endif

static void parse_line(char *line)
{
    char *eq;
    char *val;

    str_trim(line);
    if ((line[0] == '\0') || (line[0] == '#')) {
        return;
    }
    eq = strchr(line, '=');
    if (eq == NULL) {
        return;
    }
    *eq = '\0';
    val = eq + 1;
    str_trim(line);
    str_trim(val);
    kv_set(line, val);
}

static void apply_defaults(const char *mount)
{
    size_t n;

    (void)memset(s_keys, 0, sizeof(s_keys));
    (void)memset(s_files, 0, sizeof(s_files));
    s_key_n = 0U;
    s_file_n = 0U;
    s_loaded = false;
    s_mount[0] = '\0';
    s_content_path[0] = '\0';
    s_record_path[0] = '\0';

    if ((mount == NULL) || (mount[0] == '\0')) {
        mount = "/sdcard";
    }
    n = strlen(mount);
    if (n >= sizeof(s_mount)) {
        n = sizeof(s_mount) - 1U;
    }
    (void)memcpy(s_mount, mount, n);
    s_mount[n] = '\0';
    while ((n > 1U) && ((s_mount[n - 1U] == '/') || (s_mount[n - 1U] == '\\'))) {
        n--;
        s_mount[n] = '\0';
    }

    kv_set("version", "1");
    kv_set("content_root", "pet");
    kv_set("record_root", "record");
}

static void rebuild_paths(void)
{
    const char *cr = sd_cfg_get("content_root");
    const char *rr = sd_cfg_get("record_root");

    if ((cr == NULL) || !rel_ok(cr)) {
        cr = "pet";
    }
    if ((rr == NULL) || !rel_ok(rr)) {
        rr = "record";
    }
    (void)join2(s_content_path, sizeof(s_content_path), s_mount, cr);
    (void)join2(s_record_path, sizeof(s_record_path), s_mount, rr);
}

bool sd_cfg_load(const char *mount)
{
    char path[SD_CFG_PATH_MAX];
    char root_abs[SD_CFG_PATH_MAX];
    char line[192];
    FILE *fp;
    uint8_t i;
    size_t klen;

    apply_defaults(mount);
    if (!join2(path, sizeof(path), s_mount, SD_CFG_NAME)) {
        rebuild_paths();
        return false;
    }

    fp = fopen(path, "rb");
    if (fp != NULL) {
        while (fgets(line, (int)sizeof(line), fp) != NULL) {
            parse_line(line);
        }
        (void)fclose(fp);
        s_loaded = true;
    } else {
        SD_CFG_WARN("sd cfg missing %s; using defaults", path);
    }

    rebuild_paths();
    for (i = 0; i < s_key_n; i++) {
        klen = strlen(s_keys[i].key);
        if ((klen < 5U) || (strcmp(s_keys[i].key + (klen - 5U), "_root") != 0)) {
            continue;
        }
        if (!rel_ok(s_keys[i].val)) {
            continue;
        }
        if (!join2(root_abs, sizeof(root_abs), s_mount, s_keys[i].val)) {
            continue;
        }
        scan_tree(root_abs, s_keys[i].val, SD_CFG_SCAN_DEPTH);
    }
    SD_CFG_INFO("sd cfg loaded=%d files=%u content=%s",
                s_loaded ? 1 : 0, (unsigned)s_file_n, s_content_path);
    return s_loaded;
}

bool sd_cfg_is_loaded(void)
{
    return s_loaded;
}

const char *sd_cfg_mount(void)
{
    return s_mount;
}

const char *sd_cfg_get(const char *key)
{
    uint8_t i;

    if ((key == NULL) || (key[0] == '\0')) {
        return NULL;
    }
    for (i = 0; i < s_key_n; i++) {
        if (strcmp(s_keys[i].key, key) == 0) {
            return s_keys[i].val;
        }
    }
    return NULL;
}

const char *sd_cfg_content_path(void)
{
    return s_content_path;
}

const char *sd_cfg_record_path(void)
{
    return s_record_path;
}

uint8_t sd_cfg_file_count(void)
{
    return s_file_n;
}

bool sd_cfg_file_at(uint8_t index, char *rel, size_t rel_len, uint32_t *size)
{
    if ((index >= s_file_n) || (rel == NULL) || (rel_len < 2U)) {
        return false;
    }
    if (!str_copy(rel, rel_len, s_files[index].rel)) {
        return false;
    }
    if (size != NULL) {
        *size = s_files[index].size;
    }
    return true;
}
