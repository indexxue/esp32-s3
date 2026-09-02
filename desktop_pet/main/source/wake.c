/**
 * @file wake.c
 * @brief ESP-SR WakeNet on home; SD models; ding+phrase reply then auto listen.
 */

#include "wake.h"

#include "agent.h"
#include "audio.h"
#include "log.h"
#include "pet_view.h"
#include "sd_cfg.h"
#include "ui.h"
#include "web_ctrl.h"

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define WAKE_TASK_STACK (6144)
#define WAKE_TASK_PRIO (4)
#define WAKE_REPLY_POOL_MAX (8U)
#define WAKE_REPLY_NAME_LEN (24U)
#define WAKE_PATH_MAX (192)
#define WAKE_YIELD_WAIT_MS (800U)
#define WAKE_WAV_WAIT_MS (12000U)

static bool s_inited;
static bool s_available;
static volatile bool s_home_active;
static volatile bool s_holding_stream;
static volatile bool s_yield_req;
static volatile TickType_t s_pause_until;
static volatile bool s_open_chat_req;
static volatile bool s_wake_busy; /* detect→reply→listen in progress */
static volatile bool s_chat_mode; /* http suspended; WakeNet kept, detect paused */
static volatile bool s_resume_pending;
static volatile bool s_http_was_up; /* restore httpd after leave */

static char s_model_root[WAKE_PATH_MAX];
static char s_model_name_cfg[48];
static char s_reply_dir[WAKE_PATH_MAX];
static bool s_enable = true;

static const esp_wn_iface_t *s_wakenet;
static model_iface_data_t *s_wn_data;
static char *s_wn_name;
static int s_chunk_samples;
static int16_t *s_chunk_buf;
static int8_t s_reply_last = -1;

static bool cfg_truthy(const char *v)
{
    if ((v == NULL) || (v[0] == '\0')) {
        return true;
    }
    if ((v[0] == '0') && (v[1] == '\0')) {
        return false;
    }
    if ((v[0] == 'n') || (v[0] == 'N') || (v[0] == 'f') || (v[0] == 'F')) {
        return false;
    }
    return true;
}

static bool path_is_dir(const char *path)
{
    struct stat st;

    if ((path == NULL) || (path[0] == '\0')) {
        return false;
    }
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
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

static bool name_is_ding(const char *name)
{
    char a;
    char b;
    char c;
    char d;

    if ((name == NULL) || (strlen(name) < 8U)) {
        return false;
    }
    a = (char)tolower((unsigned char)name[0]);
    b = (char)tolower((unsigned char)name[1]);
    c = (char)tolower((unsigned char)name[2]);
    d = (char)tolower((unsigned char)name[3]);
    return ((a == 'd') && (b == 'i') && (c == 'n') && (d == 'g') && (name[4] == '.'));
}

static void wake_stream_stop(void)
{
    if (s_holding_stream) {
        (void)desktop_pet_audio_stream_stop();
        s_holding_stream = false;
    }
}

static bool wake_stream_start(void)
{
    if (s_holding_stream) {
        /* Agent teardown may stop I2S without clearing our flag. */
        if (desktop_pet_audio_is_streaming()) {
            return true;
        }
        s_holding_stream = false;
    }
    if (desktop_pet_audio_is_streaming() || desktop_pet_audio_is_recording() ||
        desktop_pet_audio_is_playing() || desktop_pet_audio_is_playouting()) {
        return false;
    }
    if (desktop_pet_audio_stream_start() != STATUS_OK) {
        return false;
    }
    s_holding_stream = true;
    return true;
}

static bool wake_should_detect(void)
{
    desktop_pet_agent_state_t st;
    bool blank = ui_display_is_blank();

    if (!s_available || !s_enable || !s_home_active || s_wake_busy) {
        return false;
    }
    if ((s_wn_data == NULL) || (s_chunk_buf == NULL) || (s_wakenet == NULL)) {
        return false;
    }
    if ((s_pause_until != 0U) && (xTaskGetTickCount() < s_pause_until)) {
        return false;
    }
    if (s_resume_pending) {
        return false;
    }
    if (desktop_pet_audio_is_playing() || desktop_pet_audio_is_recording() ||
        desktop_pet_audio_is_playouting()) {
        return false;
    }

    st = desktop_pet_agent_get_state();
    if ((st == DESKTOP_PET_AGENT_STATE_LISTENING) || (st == DESKTOP_PET_AGENT_STATE_SPEAKING) ||
        (st == DESKTOP_PET_AGENT_STATE_CONNECTING)) {
        return false;
    }

    /* Blank: arm WakeNet even if chat UI still open / chat_mode paused detect. */
    if (blank) {
        return true;
    }

    if (s_chat_mode) {
        return false;
    }
    if (st != DESKTOP_PET_AGENT_STATE_IDLE) {
        return false;
    }
    if (pet_view_chat_is_open() || pet_view_settings_is_open()) {
        return false;
    }
    return true;
}

/**
 * Chat enter: stop mic + suspend httpd (keep STA). Keep WakeNet instance —
 * recreate-from-SD after httpd is up leaves largest≈7KB and breaks HTTP restart too.
 */
static void wake_engine_hold(void)
{
    wake_stream_stop();
    if (web_ctrl_http_is_up()) {
        s_http_was_up = true;
        (void)web_ctrl_http_suspend();
    }
    LOG_INFO("wake: engine PAUSE (keep model, http down) free_int=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static bool wake_http_resume_if_needed(void)
{
    static TickType_t s_last_http_try;

    if (!s_http_was_up) {
        return true;
    }
    if (desktop_pet_agent_get_state() != DESKTOP_PET_AGENT_STATE_IDLE) {
        return false;
    }
    {
        TickType_t now = xTaskGetTickCount();

        if ((s_last_http_try != 0U) && ((now - s_last_http_try) < pdMS_TO_TICKS(3000))) {
            return false;
        }
        s_last_http_try = now;
    }
    if (web_ctrl_http_resume() != ESP_OK) {
        LOG_WARN("wake: HTTP resume deferred free_int=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return false;
    }
    s_http_was_up = false;
    s_last_http_try = 0U;
    LOG_INFO("wake: HTTP resumed");
    return true;
}

static bool wake_try_deferred_resume(void)
{
    if (!s_home_active || pet_view_chat_is_open() || pet_view_settings_is_open()) {
        return false;
    }
    return wake_http_resume_if_needed();
}

static void wake_wait_wav_done(void)
{
    uint32_t waited = 0U;

    while (desktop_pet_audio_is_playing() && (waited < WAKE_WAV_WAIT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20U;
    }
}

static void wake_play_abs(const char *abs)
{
    uint32_t waited = 0U;

    if ((abs == NULL) || (abs[0] == '\0')) {
        return;
    }
    /* Mic stream must be fully down before wav (codec is half-duplex). */
    while (desktop_pet_audio_is_streaming() && (waited < 500U)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10U;
    }
    if (desktop_pet_audio_play_wav_path(abs) != STATUS_OK) {
        LOG_WARN("wake: play fail %s", abs);
        return;
    }
    wake_wait_wav_done();
}

static void wake_play_reply(void)
{
    DIR *d;
    struct dirent *ent;
    char names[WAKE_REPLY_POOL_MAX][WAKE_REPLY_NAME_LEN];
    char ding_name[WAKE_REPLY_NAME_LEN];
    char abs[WAKE_PATH_MAX];
    uint8_t n = 0U;
    uint8_t pick;
    bool have_ding = false;

    ding_name[0] = '\0';
    if (s_reply_dir[0] == '\0') {
        return;
    }
    d = opendir(s_reply_dir);
    if (d == NULL) {
        LOG_WARN("wake: no reply dir %s (copy ding/phrase wavs here)", s_reply_dir);
        return;
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
        if ((len == 0U) || (len >= WAKE_REPLY_NAME_LEN)) {
            continue;
        }
        if (name_is_ding(ent->d_name)) {
            (void)memcpy(ding_name, ent->d_name, len + 1U);
            have_ding = true;
            continue;
        }
        if (n >= WAKE_REPLY_POOL_MAX) {
            continue;
        }
        (void)memcpy(names[n], ent->d_name, len + 1U);
        n++;
    }
    (void)closedir(d);

    if (!have_ding && (n == 0U)) {
        LOG_WARN("wake: empty %s (need ding.wav + phrase wavs)", s_reply_dir);
        return;
    }

    if (have_ding) {
        if (snprintf(abs, sizeof(abs), "%s/%s", s_reply_dir, ding_name) < (int)sizeof(abs)) {
            LOG_INFO("wake: ding %s", abs);
            wake_play_abs(abs);
        }
    }
    if (n == 0U) {
        return;
    }
    pick = (uint8_t)(esp_random() % (uint32_t)n);
    if ((n >= 2U) && (s_reply_last >= 0) && ((uint8_t)s_reply_last == pick)) {
        pick = (uint8_t)((pick + 1U) % n);
    }
    s_reply_last = (int8_t)pick;
    if (snprintf(abs, sizeof(abs), "%s/%s", s_reply_dir, names[pick]) < (int)sizeof(abs)) {
        LOG_INFO("wake: phrase %s", abs);
        wake_play_abs(abs);
    }
}

static void wake_probe_reply_dir(void)
{
    DIR *d;
    struct dirent *ent;
    unsigned wav_n = 0U;

    if (s_reply_dir[0] == '\0') {
        return;
    }
    if (!path_is_dir(s_reply_dir)) {
        LOG_WARN("wake: reply dir missing %s — copy tools/pet_sim/sdcard/sfx/wake/*.wav",
                 s_reply_dir);
        return;
    }
    d = opendir(s_reply_dir);
    if (d == NULL) {
        LOG_WARN("wake: reply dir open fail %s", s_reply_dir);
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        if ((ent->d_name[0] != '.') && ends_with_wav(ent->d_name)) {
            wav_n++;
        }
    }
    (void)closedir(d);
    if (wav_n == 0U) {
        LOG_WARN("wake: reply dir empty %s (need ding.wav + phrase wavs)", s_reply_dir);
    } else {
        LOG_INFO("wake: reply dir ok %s wav=%u", s_reply_dir, wav_n);
    }
}

static void wake_on_detected(void)
{
    uint32_t waited = 0U;

    s_wake_busy = true;
    wake_stream_stop();

    if (ui_display_is_blank()) {
        ui_display_blank_set(false);
    }
    ui_display_note_activity();

    /* Pause detect + free httpd stack for agent TLS (keep WakeNet). */
    s_chat_mode = true;
    wake_engine_hold();

    /* Reply before open_chat: chat_open_from_wake posts LISTEN and would race the mic. */
    wake_play_reply();
    s_open_chat_req = true;

    while (!pet_view_chat_is_open() && (waited < 3000U)) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20U;
    }

    if (pet_view_chat_is_open()) {
        (void)desktop_pet_agent_listen_start();
        LOG_INFO("wake: listen_start after reply");
    } else {
        LOG_WARN("wake: chat did not open; skip listen");
        s_chat_mode = false;
        s_resume_pending = true;
    }

    s_wake_busy = false;
}

static void wake_task(void *arg)
{
    size_t filled = 0U;

    (void)arg;
    LOG_INFO("wake: task run model=%s chunk=%d", s_wn_name ? s_wn_name : "?", s_chunk_samples);

    for (;;) {
        if (s_yield_req) {
            wake_stream_stop();
            s_yield_req = false;
            filled = 0U;
        }

        if (s_resume_pending && !s_chat_mode) {
            if (wake_try_deferred_resume()) {
                s_resume_pending = false;
                /* Drop any raced mic session from before agent/HTTP settled. */
                wake_stream_stop();
                filled = 0U;
                LOG_INFO("wake: deferred resume ok — detect armed");
            }
        }

        if (!wake_should_detect()) {
            wake_stream_stop();
            filled = 0U;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!wake_stream_start()) {
            filled = 0U;
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        /* stream_read_mono caps at 256; WakeNet needs full chunk (often 512). */
        while (filled < (size_t)s_chunk_samples) {
            size_t got = 0U;
            size_t need = (size_t)s_chunk_samples - filled;
            status_t st;

            if (!wake_should_detect() || s_yield_req) {
                break;
            }
            st = desktop_pet_audio_stream_read_mono(s_chunk_buf + filled, need, 200U, &got);
            if ((st != STATUS_OK) || (got == 0U)) {
                /* I2S fail/timeout: must yield or IDLE0 WDT trips. */
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }
            filled += got;
        }

        if (filled < (size_t)s_chunk_samples) {
            /* Incomplete chunk (gated/yield/read fail) — never busy-spin. */
            if (wake_should_detect() && !s_yield_req) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            continue;
        }
        filled = 0U;

        {
            wakenet_state_t state = s_wakenet->detect(s_wn_data, s_chunk_buf);

            if (state == WAKENET_DETECTED) {
                LOG_INFO("wake: detected (%s)", s_wn_name ? s_wn_name : "?");
                wake_on_detected();
            }
            /* Detect is CPU-heavy; yield so IDLE can pet the WDT. */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

static bool wake_load_models(void)
{
    srmodel_list_t *models;
    char *name;
    const char *filter;

    if (!path_is_dir(s_model_root)) {
        LOG_WARN("wake: no model dir %s (skip)", s_model_root);
        return false;
    }

    models = esp_srmodel_init(s_model_root);
    if ((models == NULL) || (models->num <= 0)) {
        LOG_WARN("wake: esp_srmodel_init empty at %s", s_model_root);
        return false;
    }

    filter = s_model_name_cfg;
    if ((filter[0] == '\0')) {
        filter = "hilexin";
    }
    name = esp_srmodel_filter(models, ESP_WN_PREFIX, filter);
    if (name == NULL) {
        /* fallback: any WakeNet */
        name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    }
    if (name == NULL) {
        LOG_WARN("wake: no WakeNet model matching %s", filter);
        return false;
    }

    s_wakenet = (const esp_wn_iface_t *)esp_wn_handle_from_name(name);
    if (s_wakenet == NULL) {
        LOG_WARN("wake: esp_wn_handle_from_name(%s) null", name);
        return false;
    }

    s_wn_data = s_wakenet->create(name, DET_MODE_90);
    if (s_wn_data == NULL) {
        LOG_WARN("wake: create(%s) failed", name);
        return false;
    }

    s_wn_name = name;
    s_chunk_samples = s_wakenet->get_samp_chunksize(s_wn_data);
    if (s_chunk_samples <= 0) {
        LOG_WARN("wake: bad chunksize");
        s_wakenet->destroy(s_wn_data);
        s_wn_data = NULL;
        return false;
    }

    s_chunk_buf = (int16_t *)heap_caps_malloc((size_t)s_chunk_samples * sizeof(int16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_chunk_buf == NULL) {
        s_chunk_buf = (int16_t *)heap_caps_malloc((size_t)s_chunk_samples * sizeof(int16_t),
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_chunk_buf == NULL) {
        LOG_WARN("wake: chunk buf OOM");
        s_wakenet->destroy(s_wn_data);
        s_wn_data = NULL;
        return false;
    }

    LOG_INFO("wake: model ok name=%s chunk=%d root=%s", name, s_chunk_samples, s_model_root);
    return true;
}

static void wake_read_cfg(void)
{
    const char *mount = sd_cfg_mount();
    const char *v;
    const char *mpath = "sr_models";
    const char *rpath = "sfx/wake";
    const char *mname = "wn9s_hilexin";

    s_enable = true;
    s_model_root[0] = '\0';
    s_model_name_cfg[0] = '\0';
    s_reply_dir[0] = '\0';

    if ((mount == NULL) || (mount[0] == '\0')) {
        mount = "/sdcard";
    }

    v = sd_cfg_get("wake_enable");
    s_enable = cfg_truthy(v);

    v = sd_cfg_get("wake_model_path");
    if ((v != NULL) && (v[0] != '\0')) {
        mpath = v;
    }
    v = sd_cfg_get("wake_model");
    if ((v != NULL) && (v[0] != '\0')) {
        mname = v;
    }
    v = sd_cfg_get("wake_reply_path");
    if ((v != NULL) && (v[0] != '\0')) {
        rpath = v;
    }

    (void)snprintf(s_model_name_cfg, sizeof(s_model_name_cfg), "%s", mname);
    (void)snprintf(s_model_root, sizeof(s_model_root), "%s/%s", mount, mpath);
    (void)snprintf(s_reply_dir, sizeof(s_reply_dir), "%s/%s", mount, rpath);

    v = sd_cfg_get("wake_label");
    LOG_INFO("wake: cfg enable=%d model=%s root=%s reply=%s label=%s", (int)s_enable,
             s_model_name_cfg, s_model_root, s_reply_dir, (v != NULL) ? v : "");
}

status_t desktop_pet_wake_init(void)
{
    if (s_inited) {
        return STATUS_OK;
    }
    s_inited = true;
    s_available = false;
    s_home_active = false;

    if (!desktop_pet_audio_is_ready()) {
        LOG_WARN("wake: audio not ready; skip");
        return STATUS_OK;
    }

    wake_read_cfg();
    wake_probe_reply_dir();
    if (!s_enable) {
        LOG_INFO("wake: disabled by config");
        return STATUS_OK;
    }

    if (!wake_load_models()) {
        return STATUS_OK;
    }

    if (xTaskCreate(wake_task, "pet_wake", WAKE_TASK_STACK, NULL, WAKE_TASK_PRIO, NULL) !=
        pdPASS) {
        LOG_WARN("wake: task create failed");
        s_wakenet->destroy(s_wn_data);
        s_wn_data = NULL;
        free(s_chunk_buf);
        s_chunk_buf = NULL;
        return STATUS_OK;
    }

    s_available = true;
    LOG_INFO("wake: ready (home_active=0 until splash done)");
    return STATUS_OK;
}

bool desktop_pet_wake_is_available(void)
{
    return s_available;
}

bool desktop_pet_wake_is_busy(void)
{
    return s_wake_busy;
}

void desktop_pet_wake_set_home_active(bool active)
{
    s_home_active = active;
    LOG_INFO("wake: home_active=%d", (int)active);
}

void desktop_pet_wake_enter_chat_mode(void)
{
    if (!s_available) {
        return;
    }
    s_chat_mode = true;
    s_resume_pending = false;
    s_yield_req = true;
    wake_engine_hold();
}

void desktop_pet_wake_leave_chat_mode(void)
{
    if (!s_available) {
        return;
    }
    /* Re-arm detect after HTTP resume (model was kept). */
    s_chat_mode = false;
    s_wake_busy = false;
    s_resume_pending = true;
    s_yield_req = true;
    LOG_INFO("wake: leave chat → resume pending");
}

void desktop_pet_wake_arm_for_blank(void)
{
    if (!s_available) {
        return;
    }
    /* Keep chat_mode/httpd as-is; blank path in should_detect opens the mic. */
    s_yield_req = true;
    wake_stream_stop();
    LOG_INFO("wake: armed for blank detect");
}

void desktop_pet_wake_ui_poll(void)
{
    if (!s_open_chat_req) {
        return;
    }
    s_open_chat_req = false;
    if (!pet_view_chat_is_open()) {
        pet_view_chat_open_from_wake();
    }
}

void desktop_pet_wake_yield_for_playback(void)
{
    uint32_t waited = 0U;

    if (!s_available || s_chat_mode) {
        return;
    }
    s_yield_req = true;
    /* Cover gap before play_wav sets s_wav_playing. */
    s_pause_until = xTaskGetTickCount() + pdMS_TO_TICKS(300);
    while (s_holding_stream && (waited < WAKE_YIELD_WAIT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10U;
    }
}
