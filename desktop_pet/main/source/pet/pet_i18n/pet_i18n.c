/**
 * @file pet_i18n.c
 * @brief Firmware EN/ZH UI chrome strings + KEY=VALUE overlay from skin lang/.
 */

#include "pet_i18n.h"

#include "pet_fs.h"

#if defined(ESP_PLATFORM)
#include "log.h"
#define I18N_INFO(...) LOG_INFO(__VA_ARGS__)
#define I18N_WARN(...) LOG_WARN(__VA_ARGS__)
#else
#define I18N_INFO(...) ((void)0)
#define I18N_WARN(...) ((void)0)
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PET_I18N_FILE_MAX (4096U)

typedef struct {
    const char *key;
    const char *en;
    const char *zh;
} pet_i18n_entry_t;

typedef struct {
    char key[PET_I18N_KEY_LEN];
    char val[PET_I18N_VAL_LEN];
} pet_i18n_kv_t;

static const pet_i18n_entry_t s_fw[] = {
    {"settings.title", "Settings", "设置"},
    {"settings.firmware", "Firmware", "固件"},
    {"settings.skin_pack", "Skin pack", "皮肤包"},
    {"settings.network", "Network", "网络"},
    {"settings.battery", "Battery", "电量"},
    {"settings.language", "Language", "语言"},
    {"settings.skin", "Skin", "皮肤"},
    {"settings.touch_calib", "Touch calib", "触摸校准"},
    {"settings.soon", "soon", "即将"},
    {"settings.net_off", "off", "关闭"},
    {"settings.net_ap", "AP", "热点"},
    {"settings.net_online", "online", "在线"},
    {"settings.net_offline", "offline", "离线"},
    {"settings.chg", "chg", "充"},
    {"settings.lang_en", "EN", "EN"},
    {"settings.lang_zh", "中文", "中文"},
    {"chat.tap_to_talk", "tap to talk", "点按说话"},
    {"chat.mode_listen", "listen", "听"},
    {"chat.mode_speak", "speak", "说"},
    {"chat.mode_connecting", "connecting", "连接中"},
    {"chat.mode_ready", "ready", "就绪"},
    {"home.no_pack", "NO PACK", "无皮肤"},
};

static pet_i18n_kv_t s_overlay[PET_I18N_OVERLAY_MAX];
static uint8_t s_overlay_n;
static pet_i18n_lang_t s_lang = PET_I18N_LANG_EN;

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
        if (!(isalnum((unsigned char)*k) || (*k == '_') || (*k == '.'))) {
            return false;
        }
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
        n = cap - 1U;
    }
    (void)memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

static void overlay_clear(void)
{
    s_overlay_n = 0U;
    (void)memset(s_overlay, 0, sizeof(s_overlay));
}

static void overlay_set(const char *key, const char *val)
{
    uint8_t i;

    if (!key_ok(key) || (val == NULL)) {
        return;
    }
    for (i = 0; i < s_overlay_n; i++) {
        if (strcmp(s_overlay[i].key, key) == 0) {
            (void)str_copy(s_overlay[i].val, sizeof(s_overlay[i].val), val);
            return;
        }
    }
    if (s_overlay_n >= PET_I18N_OVERLAY_MAX) {
        I18N_WARN("i18n overlay full, drop %s", key);
        return;
    }
    if (!str_copy(s_overlay[s_overlay_n].key, sizeof(s_overlay[s_overlay_n].key), key)) {
        return;
    }
    if (!str_copy(s_overlay[s_overlay_n].val, sizeof(s_overlay[s_overlay_n].val), val)) {
        return;
    }
    s_overlay_n++;
}

static const char *overlay_get(const char *key)
{
    uint8_t i;

    if (key == NULL) {
        return NULL;
    }
    for (i = 0; i < s_overlay_n; i++) {
        if (strcmp(s_overlay[i].key, key) == 0) {
            return s_overlay[i].val;
        }
    }
    return NULL;
}

static const pet_i18n_entry_t *fw_find(const char *key)
{
    size_t i;

    if (key == NULL) {
        return NULL;
    }
    for (i = 0; i < (sizeof(s_fw) / sizeof(s_fw[0])); i++) {
        if (strcmp(s_fw[i].key, key) == 0) {
            return &s_fw[i];
        }
    }
    return NULL;
}

static void parse_file(char *buf, size_t len)
{
    char *line = buf;
    char *end = buf + len;

    while (line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        char *eq;
        char key[PET_I18N_KEY_LEN];
        char val[PET_I18N_VAL_LEN];
        size_t line_len;

        if (nl != NULL) {
            *nl = '\0';
            line_len = (size_t)(nl - line);
        } else {
            line_len = (size_t)(end - line);
        }
        if ((line_len > 0U) && (line[line_len - 1U] == '\r')) {
            line[line_len - 1U] = '\0';
        }
        str_trim(line);
        if ((line[0] != '\0') && (line[0] != '#')) {
            eq = strchr(line, '=');
            if (eq != NULL) {
                *eq = '\0';
                str_trim(line);
                str_trim(eq + 1);
                if (str_copy(key, sizeof(key), line) && str_copy(val, sizeof(val), eq + 1)) {
                    overlay_set(key, val);
                }
            }
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
}

void pet_i18n_load(pet_i18n_lang_t lang)
{
    const char *rel;
    char *buf;
    size_t got = 0U;
    long sz;

    s_lang = (lang == PET_I18N_LANG_ZH) ? PET_I18N_LANG_ZH : PET_I18N_LANG_EN;
    overlay_clear();

    rel = (s_lang == PET_I18N_LANG_ZH) ? PET_I18N_REL_ZH : PET_I18N_REL_EN;
    sz = pet_fs_file_size(rel);
    if (sz <= 0) {
        I18N_INFO("i18n: no %s (firmware table)", rel);
        return;
    }
    if ((size_t)sz > PET_I18N_FILE_MAX) {
        I18N_WARN("i18n: %s too large (%ld)", rel, sz);
        return;
    }

    buf = (char *)malloc((size_t)sz + 1U);
    if (buf == NULL) {
        I18N_WARN("i18n: alloc fail");
        return;
    }
    if (!pet_fs_read_all(rel, buf, (size_t)sz + 1U, &got) || (got == 0U)) {
        free(buf);
        I18N_WARN("i18n: read fail %s", rel);
        return;
    }
    buf[got] = '\0';
    parse_file(buf, got);
    free(buf);
    I18N_INFO("i18n: loaded %s overlay=%u", rel, (unsigned)s_overlay_n);
}

pet_i18n_lang_t pet_i18n_lang(void)
{
    return s_lang;
}

const char *pet_i18n_tr_ex(const char *key, bool use_zh)
{
    const char *ov;
    const pet_i18n_entry_t *e;
    bool want_zh;

    if ((key == NULL) || (key[0] == '\0')) {
        return "";
    }

    want_zh = use_zh && (s_lang == PET_I18N_LANG_ZH);
    ov = overlay_get(key);
    if (ov != NULL) {
        /* Overlay file matches current lang load; only apply when want_zh or EN. */
        if (want_zh || (s_lang == PET_I18N_LANG_EN)) {
            return ov;
        }
    }

    e = fw_find(key);
    if (e != NULL) {
        if (want_zh && (e->zh != NULL)) {
            return e->zh;
        }
        if (e->en != NULL) {
            return e->en;
        }
    }
    return key;
}

const char *pet_i18n_tr(const char *key)
{
    return pet_i18n_tr_ex(key, true);
}
