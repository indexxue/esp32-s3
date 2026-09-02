/**
 * @file pet_sfx.c
 * @brief Enumerate pet/sfx clip WAV pools, random pick (avoid consecutive), play.
 */

#include "pet_sfx.h"

#include "audio.h"
#include "pet_fs.h"

#if defined(ESP_PLATFORM)
#include "wake.h"
#endif

#if defined(ESP_PLATFORM)
#include "log.h"
#include "esp_random.h"
#define SFX_INFO(...) LOG_INFO(__VA_ARGS__)
#define SFX_WARN(...) LOG_WARN(__VA_ARGS__)
#else
#include <stdlib.h>
#define SFX_INFO(...) ((void)0)
#define SFX_WARN(...) ((void)0)
#endif

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PET_SFX_POOL_MAX (8U)
#define PET_SFX_NAME_LEN (16U)
#define PET_SFX_REL_MAX (48U)

static bool s_chat_busy;
static int8_t s_last_idx[PET_CLIP_COUNT]; /* 0..n-1 after first play; use -1 via init flag */
static bool s_last_inited;

static void last_idx_ensure(void)
{
    uint8_t i;

    if (s_last_inited) {
        return;
    }
    for (i = 0; i < (uint8_t)PET_CLIP_COUNT; i++) {
        s_last_idx[i] = -1;
    }
    s_last_inited = true;
}

static const char *clip_dir_name(pet_clip_id_t clip)
{
    switch (clip) {
    case PET_CLIP_EAT:
        return "eat";
    case PET_CLIP_PLAY:
        return "play";
    case PET_CLIP_POKE:
        return "poke";
    case PET_CLIP_REFUSE:
        return "refuse";
    case PET_CLIP_SLEEP_LOOP:
        return "sleep";
    default:
        return NULL;
    }
}

static bool ends_with_wav(const char *name)
{
    size_t n;

    if (name == NULL) {
        return false;
    }
    n = strlen(name);
    if (n < 5U) {
        return false;
    }
    return ((name[n - 4] == '.') &&
            ((name[n - 3] == 'w') || (name[n - 3] == 'W')) &&
            ((name[n - 2] == 'a') || (name[n - 2] == 'A')) &&
            ((name[n - 1] == 'v') || (name[n - 1] == 'V')));
}

static uint32_t sfx_rand_u32(void)
{
#if defined(ESP_PLATFORM)
    return esp_random();
#else
    return (uint32_t)rand();
#endif
}

static int scan_pool(const char *dir_rel, char names[][PET_SFX_NAME_LEN], uint8_t *out_n)
{
    char abs[PET_FS_PATH_MAX];
    DIR *d;
    struct dirent *ent;
    uint8_t n = 0U;

    *out_n = 0U;
    if (!pet_fs_join(abs, sizeof(abs), dir_rel)) {
        return -1;
    }
    d = opendir(abs);
    if (d == NULL) {
        return 0;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t len;

        if (ent->d_name[0] == '.') {
            continue;
        }
        if (!ends_with_wav(ent->d_name)) {
            continue;
        }
        len = strlen(ent->d_name);
        if ((len == 0U) || (len >= PET_SFX_NAME_LEN)) {
            continue;
        }
        if (n >= PET_SFX_POOL_MAX) {
            SFX_WARN("sfx: pool full under %s", dir_rel);
            break;
        }
        (void)memcpy(names[n], ent->d_name, len + 1U);
        n++;
    }
    (void)closedir(d);
    *out_n = n;
    return (int)n;
}

void pet_sfx_set_chat_busy(bool busy)
{
    s_chat_busy = busy;
}

void pet_sfx_play_for_clip(pet_clip_id_t clip)
{
    const char *subdir;
    char dir_rel[PET_SFX_REL_MAX];
    char names[PET_SFX_POOL_MAX][PET_SFX_NAME_LEN];
    char file_rel[PET_FS_PATH_MAX];
    char abs[PET_FS_PATH_MAX];
    uint8_t n = 0U;
    uint8_t pick;
    int8_t last;

    if (s_chat_busy) {
        return;
    }
    last_idx_ensure();
    if (clip >= PET_CLIP_COUNT) {
        return;
    }
    subdir = clip_dir_name(clip);
    if (subdir == NULL) {
        return;
    }
    if (snprintf(dir_rel, sizeof(dir_rel), "sfx/%s", subdir) >= (int)sizeof(dir_rel)) {
        return;
    }
    if (scan_pool(dir_rel, names, &n) < 0) {
        return;
    }
    if (n == 0U) {
        return;
    }

    pick = (uint8_t)(sfx_rand_u32() % (uint32_t)n);
    last = s_last_idx[clip];
    if ((n >= 2U) && (last >= 0) && ((uint8_t)last == pick)) {
        pick = (uint8_t)((pick + 1U) % n);
    }
    s_last_idx[clip] = (int8_t)pick;

    if (snprintf(file_rel, sizeof(file_rel), "%s/%s", dir_rel, names[pick]) >= (int)sizeof(file_rel)) {
        return;
    }
    if (!pet_fs_join(abs, sizeof(abs), file_rel)) {
        return;
    }
#if defined(ESP_PLATFORM)
    desktop_pet_wake_yield_for_playback();
#endif
    if (desktop_pet_audio_play_wav_path(abs) != STATUS_OK) {
        SFX_WARN("sfx: play fail %s", file_rel);
        return;
    }
    SFX_INFO("sfx: %s", file_rel);
}
