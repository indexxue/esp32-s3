/**
 * @file agent.c
 * @brief 小智兼容 WS：hello + listen + Opus 二进制上行（Z1-3）。
 */

#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"

#include "audio.h"
#include "pet_opus.h"
#include "log.h"
#include "net_wifi.h"

#if CONFIG_DESKTOP_PET_AGENT_ENABLE

#define AGENT_TASK_STACK_BYTES (6144U)
/** 编码任务栈放 PSRAM：Opus 16k/60ms 栈很深。 */
#define AGENT_UPLINK_STACK_BYTES (49152U)
/** 下行解码栈放 PSRAM。 */
#define AGENT_DOWNLINK_STACK_BYTES (32768U)
/** 播放任务只写 I2S：必须内部 RAM 栈（PSRAM 栈上调 i2s_channel_write 可能假成功无声）。 */
#define AGENT_PLAYOUT_STACK_BYTES (4096U)
/** 采音任务仅 I2S，内部 RAM 小栈、高优先级，避免编码/发送堵死 DMA。 */
#define AGENT_CAPTURE_STACK_BYTES (4096U)
#define AGENT_TASK_PRIORITY (4U)
#define AGENT_UPLINK_PRIORITY (5U)
#define AGENT_DOWNLINK_PRIORITY (6U)
#define AGENT_PLAYOUT_PRIORITY (7U)
#define AGENT_CAPTURE_PRIORITY (6U)
#define AGENT_PCM_QUEUE_DEPTH (8U)
#define AGENT_OPUS_PKT_MAX (DESKTOP_PET_OPUS_MAX_PACKET)
#define AGENT_OPUS_Q_DEPTH (20U)
#define AGENT_PLAY_Q_DEPTH (12U)
#define AGENT_PLAY_PREBUF (3U)
#define AGENT_HELLO_BIT BIT0
#define AGENT_FAIL_BIT BIT1
#define AGENT_SESSION_ID_MAX (64U)
#define AGENT_HEADER_MAX (320U)
#define AGENT_CLIENT_ID_MAX (40U)

typedef struct {
    uint16_t len;
    uint8_t data[AGENT_OPUS_PKT_MAX];
} agent_opus_pkt_t;

typedef struct {
    uint16_t samples;
    int16_t pcm[DESKTOP_PET_OPUS_FRAME_SAMPLES];
} agent_play_frame_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_ws_tx_lock;
static EventGroupHandle_t s_events;
static esp_websocket_client_handle_t s_ws;
static desktop_pet_agent_state_t s_state = DESKTOP_PET_AGENT_STATE_IDLE;
static char s_session_id[AGENT_SESSION_ID_MAX];
static char s_client_id[AGENT_CLIENT_ID_MAX];
static char s_device_id[24];
static char s_headers[AGENT_HEADER_MAX];
static bool s_hello_sent;
static volatile bool s_uplink_run;
static volatile bool s_downlink_run;
static volatile bool s_playout_run;
static volatile bool s_tts_active;
static volatile bool s_listen_want;
static volatile bool s_listen_tx_armed; /* listen start 成功后才允许 Opus 上行 */
static TaskHandle_t s_uplink_task;
static TaskHandle_t s_capture_task;
static TaskHandle_t s_downlink_task;
static TaskHandle_t s_playout_task;
static QueueHandle_t s_pcm_q;
static QueueHandle_t s_opus_q;
static QueueHandle_t s_play_q;
static QueueHandle_t s_cmd_q;
static uint32_t s_uplink_frames;
static uint32_t s_downlink_frames;
static int s_server_pcm_hz = 16000;

typedef enum {
    AGENT_CMD_SESSION_TOGGLE = 1,
    AGENT_CMD_SESSION_OPEN,
    AGENT_CMD_LISTEN_START,
    AGENT_CMD_LISTEN_STOP,
    AGENT_CMD_TTS_START,
    AGENT_CMD_TTS_STOP,
    AGENT_CMD_WS_GONE,
    AGENT_CMD_SESSION_CLOSE,
} agent_cmd_t;

static bool s_inited;
static desktop_pet_agent_ui_cb_t s_ui_cb;

static QueueHandle_t agent_queue_create(UBaseType_t len, UBaseType_t item_size)
{
    return xQueueCreateWithCaps(len, item_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static int agent_ws_send_text(const char *data, int len, uint32_t timeout_ms)
{
    int n = -1;

    if (s_ws == NULL || data == NULL || len <= 0 || s_ws_tx_lock == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_ws_tx_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return -1;
    }
    if (s_ws != NULL) {
        n = esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(timeout_ms));
    }
    xSemaphoreGive(s_ws_tx_lock);
    return n;
}

static int agent_ws_send_bin(const void *data, int len, uint32_t timeout_ms)
{
    int n = -1;

    if (s_ws == NULL || data == NULL || len <= 0 || s_ws_tx_lock == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_ws_tx_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return -1;
    }
    if (s_ws != NULL) {
        n = esp_websocket_client_send_bin(s_ws, (const char *)data, len, pdMS_TO_TICKS(timeout_ms));
    }
    xSemaphoreGive(s_ws_tx_lock);
    return n;
}

static void agent_notify_ui(desktop_pet_agent_ui_evt_t evt, const char *text)
{
    desktop_pet_agent_ui_cb_t cb = s_ui_cb;

    if (cb != NULL) {
        cb(evt, text);
    }
}

static void agent_set_state(desktop_pet_agent_state_t st)
{
    s_state = st;
    agent_notify_ui(DESKTOP_PET_AGENT_UI_STATE, NULL);
}

/** 会话失败：停听意图 + ERROR + 字幕提示（可能非 LVGL 线程）。 */
static void agent_fail_session(const char *caption)
{
    s_listen_want = false;
    agent_set_state(DESKTOP_PET_AGENT_STATE_ERROR);
    if ((caption != NULL) && (caption[0] != '\0')) {
        agent_notify_ui(DESKTOP_PET_AGENT_UI_NET, caption);
    }
}

static void agent_fill_ids(void)
{
    uint8_t mac[6] = {0};

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        (void)esp_wifi_get_mac(WIFI_IF_STA, mac);
    }

    (void)snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                   mac[3], mac[4], mac[5]);
    (void)snprintf(s_client_id, sizeof(s_client_id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[0] ^ 0x5Au, mac[1] ^ 0xA5u, mac[2], mac[3],
                   mac[4], mac[5], mac[0], mac[1], mac[2], mac[3]);
}

static void agent_stop_uplink(void)
{
    s_uplink_run = false;
    s_listen_tx_armed = false;
    for (int i = 0; i < 80 && (s_uplink_task != NULL || s_capture_task != NULL); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_pcm_q != NULL) {
        xQueueReset(s_pcm_q);
    }
    (void)desktop_pet_audio_stream_stop();
    desktop_pet_opus_enc_deinit();
}

static void agent_stop_downlink(void)
{
    s_tts_active = false;
    s_downlink_run = false;
    s_playout_run = false;
    for (int i = 0; i < 100 && (s_downlink_task != NULL || s_playout_task != NULL); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_opus_q != NULL) {
        xQueueReset(s_opus_q);
    }
    if (s_play_q != NULL) {
        xQueueReset(s_play_q);
    }
    (void)desktop_pet_audio_playout_stop();
    desktop_pet_opus_dec_deinit();
}

static void agent_mute_uplink_fast(void)
{
    /* WS 回调里立刻禁发，避免 TTS 下行时仍 send_bin 把连接写爆。 */
    s_listen_tx_armed = false;
    s_uplink_run = false;
}

static void agent_destroy_ws(void)
{
    agent_stop_uplink();
    agent_stop_downlink();
    if (s_ws == NULL) {
        return;
    }

    /* 已断开时只 destroy，避免 stop() 卡满 network_timeout（曾见 ~10s）。 */
    if (esp_websocket_client_is_connected(s_ws)) {
        (void)esp_websocket_client_close(s_ws, pdMS_TO_TICKS(200));
        (void)esp_websocket_client_stop(s_ws);
    }
    (void)esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
}

static status_t agent_send_listen(const char *state)
{
    char json[192];
    int n;

    if (s_ws == NULL || state == NULL) {
        return STATUS_INVALID_ARG;
    }

    n = snprintf(json, sizeof(json),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"manual\"}", s_session_id,
                 state);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return STATUS_FAIL;
    }
    if (agent_ws_send_text(json, n, 2000U) < 0) {
        LOG_ERROR("agent: send listen %s failed", state);
        return STATUS_FAIL;
    }
    LOG_INFO("agent: listen %s", state);
    return STATUS_OK;
}

static void agent_capture_task(void *arg)
{
    static int16_t s_pcm[DESKTOP_PET_OPUS_FRAME_SAMPLES];
    size_t acc = 0U;
    uint32_t read_fail = 0U;
    uint32_t drop_frames = 0U;

    (void)arg;
    LOG_INFO("agent: capture start");

    while (s_uplink_run) {
        size_t got = 0U;
        size_t need;
        status_t st;

        if (!desktop_pet_audio_is_streaming()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        need = DESKTOP_PET_OPUS_FRAME_SAMPLES - acc;
        st = desktop_pet_audio_stream_read_mono(s_pcm + acc, need, 100U, &got);
        if (!s_uplink_run) {
            break;
        }
        if (st != STATUS_OK || got == 0U) {
            read_fail++;
            if ((read_fail % 100U) == 1U) {
                LOG_WARN("agent: capture read_fail=%u", (unsigned)read_fail);
            }
            /* 等下一截 PCM，避免 2ms 空转抢 CPU 拖垮 WiFi TX。 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        acc += got;
        if (acc < DESKTOP_PET_OPUS_FRAME_SAMPLES) {
            continue;
        }
        acc = 0U;

        if (s_pcm_q == NULL) {
            continue;
        }
        if (xQueueSend(s_pcm_q, s_pcm, 0) != pdTRUE) {
            drop_frames++;
        }
    }

    s_capture_task = NULL;
    LOG_INFO("agent: capture end read_fail=%u drop=%u", (unsigned)read_fail, (unsigned)drop_frames);
    vTaskDelete(NULL);
}

static void agent_uplink_task(void *arg)
{
    static int16_t s_pcm[DESKTOP_PET_OPUS_FRAME_SAMPLES];
    static uint8_t s_opus_pkt[DESKTOP_PET_OPUS_MAX_PACKET];
    uint32_t send_fail = 0U;

    (void)arg;
    s_uplink_frames = 0U;
    LOG_INFO("agent: uplink start");

    while (s_uplink_run) {
        size_t opus_len = 0U;
        status_t st;

        if (s_pcm_q == NULL ||
            xQueueReceive(s_pcm_q, s_pcm, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!s_uplink_run || s_ws == NULL) {
            break;
        }

        st = desktop_pet_opus_encode_frame(s_pcm, s_opus_pkt, sizeof(s_opus_pkt), &opus_len);
        if (st != STATUS_OK || opus_len == 0U) {
            LOG_WARN("agent: opus encode fail st=%s len=%u", status_to_str(st), (unsigned)opus_len);
            continue;
        }

        if (!s_uplink_run) {
            break;
        }
        if (!s_listen_tx_armed) {
            continue;
        }
        /*
         * 短超时丢帧；managed websocket 已改：wlen==0 不再 abort。
         * 超时过长会堵编码队列（drop 飙升），多轮后更易雪崩。
         */
        if (agent_ws_send_bin(s_opus_pkt, (int)opus_len, 400U) <= 0) {
            send_fail++;
            if ((send_fail % 10U) == 1U) {
                LOG_WARN("agent: opus send fail/drop count=%u connected=%d", (unsigned)send_fail,
                         (s_ws != NULL) ? (int)esp_websocket_client_is_connected(s_ws) : 0);
            }
            continue;
        }
        s_uplink_frames++;
        if ((s_uplink_frames % 50U) == 1U) {
            LOG_INFO("agent: uplink frames=%u last_opus=%u", (unsigned)s_uplink_frames, (unsigned)opus_len);
        }
    }

    s_uplink_task = NULL;
    LOG_INFO("agent: uplink end frames=%u send_fail=%u", (unsigned)s_uplink_frames, (unsigned)send_fail);
    vTaskDeleteWithCaps(NULL);
}

static status_t agent_start_listening(void)
{
    status_t st;

    /* 多轮后必须先清净下行/播放，再开采，避免 I2S/队列残留把上行拖死。 */
    agent_stop_downlink();

    if (desktop_pet_opus_enc_init() != STATUS_OK) {
        return STATUS_FAIL;
    }
    st = desktop_pet_audio_stream_start();
    if (st != STATUS_OK) {
        desktop_pet_opus_enc_deinit();
        return st;
    }

    if (s_pcm_q == NULL) {
        s_pcm_q = agent_queue_create(AGENT_PCM_QUEUE_DEPTH,
                                     DESKTOP_PET_OPUS_FRAME_SAMPLES * sizeof(int16_t));
        if (s_pcm_q == NULL) {
            (void)desktop_pet_audio_stream_stop();
            desktop_pet_opus_enc_deinit();
            LOG_ERROR("agent: pcm queue create failed");
            return STATUS_NO_MEM;
        }
    } else {
        xQueueReset(s_pcm_q);
    }

    /*
     * 必须先 listen start，再开上行。
     * 否则服务端在未 listen 时收到 Opus，易直接掐断（表现为 transport_poll_write / 客户端断开）。
     */
    s_listen_tx_armed = false;
    if (agent_send_listen("start") != STATUS_OK) {
        (void)desktop_pet_audio_stream_stop();
        desktop_pet_opus_enc_deinit();
        return STATUS_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(40));

    s_uplink_run = true;
    if (xTaskCreate(agent_capture_task, "pet_cap", AGENT_CAPTURE_STACK_BYTES, NULL, AGENT_CAPTURE_PRIORITY,
                    &s_capture_task) != pdPASS) {
        s_uplink_run = false;
        s_capture_task = NULL;
        (void)desktop_pet_audio_stream_stop();
        desktop_pet_opus_enc_deinit();
        LOG_ERROR("agent: capture task create failed");
        return STATUS_FAIL;
    }
    if (xTaskCreateWithCaps(agent_uplink_task, "pet_up", AGENT_UPLINK_STACK_BYTES, NULL, AGENT_UPLINK_PRIORITY,
                            &s_uplink_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_uplink_run = false;
        for (int i = 0; i < 50 && s_capture_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        (void)desktop_pet_audio_stream_stop();
        desktop_pet_opus_enc_deinit();
        LOG_ERROR("agent: uplink task create failed");
        return STATUS_FAIL;
    }

    s_listen_tx_armed = true;
    agent_set_state(DESKTOP_PET_AGENT_STATE_LISTENING);
    LOG_INFO("agent: LISTENING");
    return STATUS_OK;
}

static size_t agent_resample_to_16k(const int16_t *in, size_t in_n, int in_hz, int16_t *out, size_t out_cap)
{
    size_t out_n;
    size_t j;

    if (in == NULL || out == NULL || in_n == 0U || out_cap == 0U) {
        return 0U;
    }
    if (in_hz == (int)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ) {
        out_n = (in_n < out_cap) ? in_n : out_cap;
        memcpy(out, in, out_n * sizeof(int16_t));
        return out_n;
    }
    if (in_hz <= 0) {
        return 0U;
    }

    out_n = (size_t)((in_n * (size_t)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ) / (size_t)in_hz);
    if (out_n > out_cap) {
        out_n = out_cap;
    }
    for (j = 0U; j < out_n; j++) {
        size_t src = (j * (size_t)in_hz) / (size_t)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ;

        if (src >= in_n) {
            src = in_n - 1U;
        }
        out[j] = in[src];
    }
    return out_n;
}

static void agent_playout_task(void *arg)
{
    /* 帧较大，放静态区，避免任务栈被吃光。 */
    static agent_play_frame_t s_frame;
    bool primed = false;
    uint32_t underrun = 0U;
    uint32_t write_fail = 0U;

    (void)arg;
    LOG_INFO("agent: playout task start (internal stack)");

    while (s_playout_run || (s_play_q != NULL && uxQueueMessagesWaiting(s_play_q) > 0U)) {
        if (!primed) {
            if (s_play_q == NULL || uxQueueMessagesWaiting(s_play_q) < AGENT_PLAY_PREBUF) {
                if (!s_playout_run && (s_play_q == NULL || uxQueueMessagesWaiting(s_play_q) == 0U)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            primed = true;
            LOG_INFO("agent: playout primed %u frames", (unsigned)AGENT_PLAY_PREBUF);
        }

        if (s_play_q == NULL ||
            xQueueReceive(s_play_q, &s_frame, pdMS_TO_TICKS(40)) != pdTRUE) {
            if (!s_playout_run) {
                break;
            }
            underrun++;
            continue;
        }

        if (s_frame.samples == 0U) {
            continue;
        }
        if (desktop_pet_audio_playout_write_mono(s_frame.pcm, s_frame.samples, 500U) != STATUS_OK) {
            write_fail++;
            if ((write_fail % 10U) == 1U) {
                LOG_WARN("agent: playout write fail count=%u", (unsigned)write_fail);
            }
        }
    }

    s_playout_task = NULL;
    LOG_INFO("agent: playout task end underrun=%u write_fail=%u", (unsigned)underrun, (unsigned)write_fail);
    vTaskDelete(NULL);
}

static void agent_downlink_task(void *arg)
{
    static int16_t s_pcm_raw[DESKTOP_PET_OPUS_PCM_MAX_SAMPLES];
    static int16_t s_pcm_16k[DESKTOP_PET_OPUS_PCM_MAX_SAMPLES];
    agent_opus_pkt_t pkt;
    agent_play_frame_t frame;
    uint32_t dec_fail = 0U;
    uint32_t drop = 0U;

    (void)arg;
    s_downlink_frames = 0U;
    LOG_INFO("agent: downlink start");

    while (s_downlink_run) {
        size_t got = 0U;
        size_t play_n;
        status_t st;

        if (s_opus_q == NULL || xQueueReceive(s_opus_q, &pkt, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!s_downlink_run) {
            break;
        }

        st = desktop_pet_opus_decode_frame(pkt.data, pkt.len, s_pcm_raw, DESKTOP_PET_OPUS_PCM_MAX_SAMPLES, &got);
        if (st != STATUS_OK || got == 0U) {
            dec_fail++;
            continue;
        }

        play_n = agent_resample_to_16k(s_pcm_raw, got, desktop_pet_opus_dec_sample_rate(), s_pcm_16k,
                                       DESKTOP_PET_OPUS_FRAME_SAMPLES);
        if (play_n == 0U) {
            continue;
        }
        if (play_n > DESKTOP_PET_OPUS_FRAME_SAMPLES) {
            play_n = DESKTOP_PET_OPUS_FRAME_SAMPLES;
        }

        frame.samples = (uint16_t)play_n;
        memcpy(frame.pcm, s_pcm_16k, play_n * sizeof(int16_t));
        if (s_play_q != NULL) {
            if (xQueueSend(s_play_q, &frame, pdMS_TO_TICKS(40)) != pdTRUE) {
                agent_play_frame_t dump;
                (void)xQueueReceive(s_play_q, &dump, 0);
                if (xQueueSend(s_play_q, &frame, 0) != pdTRUE) {
                    drop++;
                }
            }
        }

        s_downlink_frames++;
        if ((s_downlink_frames % 50U) == 1U) {
            LOG_INFO("agent: downlink frames=%u pcm=%u q=%u", (unsigned)s_downlink_frames, (unsigned)play_n,
                     (unsigned)(s_play_q ? uxQueueMessagesWaiting(s_play_q) : 0U));
        }
    }

    s_downlink_task = NULL;
    LOG_INFO("agent: downlink end frames=%u dec_fail=%u drop=%u", (unsigned)s_downlink_frames, (unsigned)dec_fail,
             (unsigned)drop);
    vTaskDeleteWithCaps(NULL);
}

static status_t agent_tts_start(void)
{
    /* 半双工：说前硬停上行，避免听写任务/锁残留。 */
    agent_stop_uplink();
    if (s_state == DESKTOP_PET_AGENT_STATE_LISTENING) {
        (void)agent_send_listen("stop");
        agent_set_state(DESKTOP_PET_AGENT_STATE_OPEN);
    }

    if (s_opus_q == NULL) {
        s_opus_q = agent_queue_create(AGENT_OPUS_Q_DEPTH, sizeof(agent_opus_pkt_t));
        if (s_opus_q == NULL) {
            LOG_ERROR("agent: opus queue create failed");
            s_tts_active = false;
            return STATUS_NO_MEM;
        }
    }

    if (desktop_pet_opus_dec_init(s_server_pcm_hz) != STATUS_OK) {
        s_tts_active = false;
        return STATUS_FAIL;
    }
    if (desktop_pet_audio_playout_start() != STATUS_OK) {
        desktop_pet_opus_dec_deinit();
        s_tts_active = false;
        return STATUS_FAIL;
    }

    if (s_play_q == NULL) {
        s_play_q = agent_queue_create(AGENT_PLAY_Q_DEPTH, sizeof(agent_play_frame_t));
        if (s_play_q == NULL) {
            (void)desktop_pet_audio_playout_stop();
            desktop_pet_opus_dec_deinit();
            s_tts_active = false;
            LOG_ERROR("agent: play queue create failed");
            return STATUS_NO_MEM;
        }
    } else {
        xQueueReset(s_play_q);
    }

    if (s_playout_task == NULL) {
        s_playout_run = true;
        if (xTaskCreate(agent_playout_task, "pet_ao", AGENT_PLAYOUT_STACK_BYTES, NULL, AGENT_PLAYOUT_PRIORITY,
                        &s_playout_task) != pdPASS) {
            s_playout_run = false;
            s_playout_task = NULL;
            s_tts_active = false;
            (void)desktop_pet_audio_playout_stop();
            desktop_pet_opus_dec_deinit();
            LOG_ERROR("agent: playout task create failed");
            return STATUS_FAIL;
        }
    }

    if (s_downlink_task == NULL) {
        s_downlink_run = true;
        if (xTaskCreateWithCaps(agent_downlink_task, "pet_dn", AGENT_DOWNLINK_STACK_BYTES, NULL,
                                AGENT_DOWNLINK_PRIORITY, &s_downlink_task,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            s_downlink_run = false;
            s_downlink_task = NULL;
            s_playout_run = false;
            for (int i = 0; i < 50 && s_playout_task != NULL; i++) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            s_tts_active = false;
            (void)desktop_pet_audio_playout_stop();
            desktop_pet_opus_dec_deinit();
            LOG_ERROR("agent: downlink task create failed");
            return STATUS_FAIL;
        }
    }

    agent_set_state(DESKTOP_PET_AGENT_STATE_SPEAKING);
    LOG_INFO("agent: SPEAKING (tts start) sr=%d", s_server_pcm_hz);
    return STATUS_OK;
}

static void agent_tts_stop(void)
{
    int wait;

    s_tts_active = false;
    /* 先让 opus 队列排空进 play 队列，再停。 */
    for (wait = 0; wait < 40 && s_opus_q != NULL && uxQueueMessagesWaiting(s_opus_q) > 0U; wait++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_downlink_run = false;
    for (wait = 0; wait < 40 && s_play_q != NULL && uxQueueMessagesWaiting(s_play_q) > 0U; wait++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    agent_stop_downlink();
    if (s_state == DESKTOP_PET_AGENT_STATE_SPEAKING) {
        agent_set_state(DESKTOP_PET_AGENT_STATE_OPEN);
    }
    LOG_INFO("agent: tts stop -> OPEN");
}

static status_t agent_post_cmd(agent_cmd_t cmd)
{
    if (s_cmd_q == NULL) {
        return STATUS_INVALID_STATE;
    }
    if (xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE) {
        return STATUS_OK;
    }
    /* STOP 必须送达：清队列后重投（启停冲突时以停止为准）。 */
    if (cmd == AGENT_CMD_LISTEN_STOP || cmd == AGENT_CMD_TTS_STOP || cmd == AGENT_CMD_SESSION_TOGGLE ||
        cmd == AGENT_CMD_WS_GONE || cmd == AGENT_CMD_SESSION_CLOSE) {
        xQueueReset(s_cmd_q);
        if (xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE) {
            return STATUS_OK;
        }
    }
    LOG_WARN("agent: cmd queue full cmd=%d", (int)cmd);
    return STATUS_FAIL;
}

static void agent_enqueue_opus(const uint8_t *data, int len)
{
    agent_opus_pkt_t pkt;

    if (!s_tts_active || data == NULL || len <= 0 || s_opus_q == NULL) {
        return;
    }
    if ((size_t)len > sizeof(pkt.data)) {
        LOG_WARN("agent: opus pkt too big %d", len);
        return;
    }
    pkt.len = (uint16_t)len;
    memcpy(pkt.data, data, (size_t)len);
    (void)xQueueSend(s_opus_q, &pkt, 0);
}

static void agent_handle_text(const char *data, int len)
{
    cJSON *root;
    const cJSON *type;
    const cJSON *transport;
    const cJSON *sid;
    const cJSON *text;
    const cJSON *state;
    const cJSON *audio;
    const cJSON *sr;
    char *tmp;

    if (data == NULL || len <= 0) {
        return;
    }

    tmp = (char *)malloc((size_t)len + 1U);
    if (tmp == NULL) {
        return;
    }
    memcpy(tmp, data, (size_t)len);
    tmp[len] = '\0';

    root = cJSON_Parse(tmp);
    free(tmp);
    if (root == NULL) {
        LOG_WARN("agent: bad json");
        return;
    }

    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
        if (cJSON_IsString(transport) && transport->valuestring != NULL &&
            strcmp(transport->valuestring, "websocket") == 0) {
            sid = cJSON_GetObjectItemCaseSensitive(root, "session_id");
            if (cJSON_IsString(sid) && sid->valuestring != NULL) {
                strncpy(s_session_id, sid->valuestring, sizeof(s_session_id) - 1U);
                s_session_id[sizeof(s_session_id) - 1U] = '\0';
            } else {
                s_session_id[0] = '\0';
            }
            audio = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
            if (cJSON_IsObject(audio)) {
                sr = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
                if (cJSON_IsNumber(sr) && sr->valuedouble > 0.0) {
                    s_server_pcm_hz = (int)sr->valuedouble;
                }
            }
            LOG_INFO("agent: hello ok session_id=%s server_sr=%d",
                     s_session_id[0] != '\0' ? s_session_id : "(none)", s_server_pcm_hz);
            (void)xEventGroupSetBits(s_events, AGENT_HELLO_BIT);
        }
    } else if (strcmp(type->valuestring, "stt") == 0) {
        text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            LOG_INFO("agent: STT \"%s\"", text->valuestring);
            agent_notify_ui(DESKTOP_PET_AGENT_UI_STT, text->valuestring);
        }
    } else if (strcmp(type->valuestring, "tts") == 0) {
        state = cJSON_GetObjectItemCaseSensitive(root, "state");
        text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL && text->valuestring[0] != '\0') {
            agent_notify_ui(DESKTOP_PET_AGENT_UI_TTS_TEXT, text->valuestring);
        }
        if (cJSON_IsString(state) && state->valuestring != NULL) {
            LOG_INFO("agent: tts state=%s", state->valuestring);
            if (strcmp(state->valuestring, "start") == 0) {
                /* 半双工：一收到 tts start 立刻停上行，再交给 worker 做 listen stop / 播报。 */
                agent_mute_uplink_fast();
                s_tts_active = true;
                if (s_opus_q == NULL) {
                    s_opus_q = agent_queue_create(AGENT_OPUS_Q_DEPTH, sizeof(agent_opus_pkt_t));
                }
                (void)agent_post_cmd(AGENT_CMD_TTS_START);
            } else if (strcmp(state->valuestring, "stop") == 0) {
                (void)agent_post_cmd(AGENT_CMD_TTS_STOP);
            }
        } else {
            LOG_INFO("agent: rx type=tts");
        }
    } else if (strcmp(type->valuestring, "llm") == 0) {
        text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            LOG_INFO("agent: LLM \"%s\"", text->valuestring);
            agent_notify_ui(DESKTOP_PET_AGENT_UI_LLM, text->valuestring);
        } else {
            LOG_INFO("agent: rx type=llm");
        }
    } else {
        LOG_INFO("agent: rx type=%s", type->valuestring);
    }

    cJSON_Delete(root);
}

static void agent_ws_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    (void)handler_args;
    (void)base;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        LOG_INFO("agent: ws connected, send hello");
        if (!s_hello_sent && s_ws != NULL) {
            static const char hello_json[] =
                "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
                "\"features\":{\"mcp\":false,\"aec\":false},"
                "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
            const int n = agent_ws_send_text(hello_json, (int)strlen(hello_json), 3000U);
            if (n < 0) {
                LOG_ERROR("agent: send hello failed");
                (void)xEventGroupSetBits(s_events, AGENT_FAIL_BIT);
            } else {
                s_hello_sent = true;
            }
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
            break;
        }
        if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            agent_handle_text(data->data_ptr, data->data_len);
        } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
            /* 下行音频到来时若仍在听，先静音上行（防半双工抢写）。 */
            if (s_listen_tx_armed || (s_state == DESKTOP_PET_AGENT_STATE_LISTENING)) {
                agent_mute_uplink_fast();
            }
            /* 仅完整帧入队；分片无拼包缓冲，丢弃避免坏 Opus。 */
            if (data->payload_offset == 0 && data->data_len == data->payload_len) {
                agent_enqueue_opus((const uint8_t *)data->data_ptr, data->data_len);
            } else if (data->payload_len > 0) {
                LOG_WARN("agent: drop fragmented opus off=%d chunk=%d total=%d", data->payload_offset,
                         data->data_len, data->payload_len);
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        LOG_WARN("agent: ws closed/disconnected");
        agent_mute_uplink_fast();
        s_tts_active = false;
        s_downlink_run = false;
        s_playout_run = false;
        /* 勿清 s_listen_want：对话页仍开着时要靠它自动重连进听。 */
        if (s_state == DESKTOP_PET_AGENT_STATE_CONNECTING) {
            (void)xEventGroupSetBits(s_events, AGENT_FAIL_BIT);
        } else if (s_state == DESKTOP_PET_AGENT_STATE_OPEN || s_state == DESKTOP_PET_AGENT_STATE_LISTENING ||
                   s_state == DESKTOP_PET_AGENT_STATE_SPEAKING) {
            /* 停 I2S/Opus 必须在 worker，禁止在 WS 任务里直接 teardown。 */
            (void)agent_post_cmd(AGENT_CMD_WS_GONE);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        /* 停听过程中的写失败常伴随 ERROR，随后会 CLOSED；CONNECTING 才当握手失败。 */
        if (s_state == DESKTOP_PET_AGENT_STATE_CONNECTING) {
            LOG_ERROR("agent: ws error while connecting");
            (void)xEventGroupSetBits(s_events, AGENT_FAIL_BIT);
        } else {
            LOG_WARN("agent: ws error (state=%d)", (int)s_state);
        }
        break;

    default:
        break;
    }
}

static status_t agent_open_session(void)
{
    EventBits_t bits;
    esp_websocket_client_config_t cfg = {0};
    esp_err_t err;
    int n;

    if (!net_wifi_sta_has_ipv4()) {
        LOG_ERROR("agent: STA has no IPv4 (join LAN WiFi first)");
        agent_fail_session("no WiFi — join LAN");
        return STATUS_INVALID_STATE;
    }

    agent_destroy_ws();

    agent_set_state(DESKTOP_PET_AGENT_STATE_CONNECTING);
    s_hello_sent = false;
    s_session_id[0] = '\0';
    (void)xEventGroupClearBits(s_events, AGENT_HELLO_BIT | AGENT_FAIL_BIT);

    agent_fill_ids();

    n = snprintf(s_headers, sizeof(s_headers),
                 "Authorization: Bearer %s\r\n"
                 "Protocol-Version: 1\r\n"
                 "Device-Id: %s\r\n"
                 "Client-Id: %s\r\n",
                 CONFIG_DESKTOP_PET_AGENT_ACCESS_TOKEN, s_device_id, s_client_id);
    if (n <= 0 || (size_t)n >= sizeof(s_headers)) {
        agent_fail_session("connect fail");
        return STATUS_FAIL;
    }

    cfg.uri = CONFIG_DESKTOP_PET_AGENT_WS_URI;
    cfg.headers = s_headers;
    cfg.buffer_size = 4096;
    /* 栈在 PSRAM（managed esp_websocket_client 已改 WithCaps）；16K 防 send_bin 溢出。 */
    cfg.task_stack = 16384;
    cfg.reconnect_timeout_ms = 10000;
    cfg.network_timeout_ms = 10000;
    cfg.disable_auto_reconnect = true;
    cfg.disable_pingpong_discon = true;
    cfg.ping_interval_sec = 30;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 5;
    cfg.keep_alive_interval = 3;
    cfg.keep_alive_count = 3;

    LOG_INFO("agent: connecting %s device_id=%s free_int=%u largest_int=%u", CONFIG_DESKTOP_PET_AGENT_WS_URI,
             s_device_id, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        LOG_ERROR("agent: ws init failed");
        agent_fail_session("connect fail");
        return STATUS_NO_MEM;
    }

    err = esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, agent_ws_event, NULL);
    if (err != ESP_OK) {
        LOG_ERROR("agent: register events: %s", esp_err_to_name(err));
        agent_destroy_ws();
        agent_fail_session("connect fail");
        return err;
    }

    err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        LOG_ERROR("agent: ws start: %s", esp_err_to_name(err));
        agent_destroy_ws();
        agent_fail_session("connect fail");
        return err;
    }

    bits = xEventGroupWaitBits(s_events, AGENT_HELLO_BIT | AGENT_FAIL_BIT, pdTRUE, pdFALSE,
                               pdMS_TO_TICKS(CONFIG_DESKTOP_PET_AGENT_HELLO_TIMEOUT_MS));
    if ((bits & AGENT_HELLO_BIT) == 0U) {
        LOG_ERROR("agent: hello timeout/fail");
        agent_destroy_ws();
        agent_fail_session("server timeout");
        return STATUS_TIMEOUT;
    }

    agent_set_state(DESKTOP_PET_AGENT_STATE_OPEN);
    LOG_INFO("agent: session OPEN");
    return STATUS_OK;
}

static status_t agent_stop_listening(void)
{
    if (s_state != DESKTOP_PET_AGENT_STATE_LISTENING) {
        return STATUS_OK;
    }

    /* 必须先停上行，再发 listen stop，避免 send_bin/send_text 并发弄死 WS。 */
    agent_stop_uplink();
    (void)agent_send_listen("stop");
    if (s_ws != NULL && s_session_id[0] != '\0') {
        agent_set_state(DESKTOP_PET_AGENT_STATE_OPEN);
        LOG_INFO("agent: session OPEN (listen stopped)");
    } else {
        agent_set_state(DESKTOP_PET_AGENT_STATE_IDLE);
    }
    return STATUS_OK;
}

static void agent_close_session(void)
{
    s_listen_want = false;
    if (s_ws != NULL && (s_state == DESKTOP_PET_AGENT_STATE_LISTENING || s_state == DESKTOP_PET_AGENT_STATE_OPEN ||
                         s_state == DESKTOP_PET_AGENT_STATE_SPEAKING)) {
        (void)agent_send_listen("stop");
    }
    agent_destroy_ws();
    s_session_id[0] = '\0';
    s_hello_sent = false;
    agent_set_state(DESKTOP_PET_AGENT_STATE_IDLE);
    LOG_INFO("agent: session IDLE");
}

static void agent_worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        agent_cmd_t cmd = (agent_cmd_t)0;

        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20000)) != pdTRUE) {
            LOG_WARN("agent: worker lock timeout cmd=%d", (int)cmd);
            continue;
        }

        switch (cmd) {
        case AGENT_CMD_SESSION_TOGGLE:
            if (s_state == DESKTOP_PET_AGENT_STATE_OPEN || s_state == DESKTOP_PET_AGENT_STATE_LISTENING ||
                s_state == DESKTOP_PET_AGENT_STATE_SPEAKING || s_state == DESKTOP_PET_AGENT_STATE_CONNECTING) {
                agent_close_session();
            } else {
                status_t st = agent_open_session();
                if (st != STATUS_OK) {
                    LOG_WARN("agent: session open %s", status_to_str(st));
                }
            }
            break;

        case AGENT_CMD_SESSION_OPEN:
            if (s_state == DESKTOP_PET_AGENT_STATE_OPEN || s_state == DESKTOP_PET_AGENT_STATE_LISTENING ||
                s_state == DESKTOP_PET_AGENT_STATE_SPEAKING || s_state == DESKTOP_PET_AGENT_STATE_CONNECTING) {
                break;
            }
            {
                status_t st = agent_open_session();
                if (st != STATUS_OK) {
                    LOG_WARN("agent: session open %s", status_to_str(st));
                }
            }
            break;

        case AGENT_CMD_SESSION_CLOSE:
            agent_close_session();
            break;

        case AGENT_CMD_LISTEN_START:
            if (!s_listen_want) {
                LOG_INFO("agent: listen start cancelled");
                break;
            }
            if (s_state == DESKTOP_PET_AGENT_STATE_LISTENING) {
                break;
            }
            if (s_state == DESKTOP_PET_AGENT_STATE_SPEAKING) {
                /* 打断：立刻停播，勿等 TTS 队列排空。 */
                s_tts_active = false;
                agent_stop_downlink();
                if (s_state == DESKTOP_PET_AGENT_STATE_SPEAKING) {
                    agent_set_state(DESKTOP_PET_AGENT_STATE_OPEN);
                }
            }
            if (s_state != DESKTOP_PET_AGENT_STATE_OPEN) {
                status_t st = agent_open_session();
                if (st != STATUS_OK) {
                    LOG_WARN("agent: listen needs session: %s", status_to_str(st));
                    s_listen_want = false;
                    /* agent_open_session 已发 NET 字幕；保持 ERROR。 */
                    break;
                }
            }
            if (s_listen_want && s_state == DESKTOP_PET_AGENT_STATE_OPEN) {
                status_t st = agent_start_listening();
                if (st != STATUS_OK) {
                    LOG_WARN("agent: listen start failed %s", status_to_str(st));
                    s_listen_want = false;
                    agent_notify_ui(DESKTOP_PET_AGENT_UI_NET, "listen fail");
                }
            } else if (!s_listen_want) {
                LOG_INFO("agent: listen start cancelled before uplink");
            }
            break;

        case AGENT_CMD_LISTEN_STOP:
            s_listen_want = false;
            if (s_state == DESKTOP_PET_AGENT_STATE_LISTENING) {
                (void)agent_stop_listening();
            }
            break;

        case AGENT_CMD_TTS_START:
            (void)agent_tts_start();
            break;

        case AGENT_CMD_TTS_STOP:
            agent_tts_stop();
            break;

        case AGENT_CMD_WS_GONE:
            s_listen_want = false;
            agent_stop_uplink();
            agent_stop_downlink();
            if (s_ws != NULL) {
                if (esp_websocket_client_is_connected(s_ws)) {
                    (void)esp_websocket_client_close(s_ws, pdMS_TO_TICKS(200));
                    (void)esp_websocket_client_stop(s_ws);
                }
                (void)esp_websocket_client_destroy(s_ws);
                s_ws = NULL;
            }
            s_session_id[0] = '\0';
            s_hello_sent = false;
            agent_set_state(DESKTOP_PET_AGENT_STATE_IDLE);
            agent_notify_ui(DESKTOP_PET_AGENT_UI_NET, "disconnected");
            LOG_INFO("agent: session IDLE (ws gone) free_int=%u largest=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            break;

        default:
            break;
        }

        xSemaphoreGive(s_lock);
    }
}

static void agent_init_teardown(void)
{
    if (s_cmd_q != NULL) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
    }
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    if (s_ws_tx_lock != NULL) {
        vSemaphoreDelete(s_ws_tx_lock);
        s_ws_tx_lock = NULL;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    s_inited = false;
}

status_t desktop_pet_agent_init(void)
{
    if (s_inited) {
        return STATUS_OK;
    }

    /* 上次半初始化留下的句柄：清掉再重试。 */
    if (s_lock != NULL || s_ws_tx_lock != NULL || s_events != NULL || s_cmd_q != NULL) {
        agent_init_teardown();
    }

    s_lock = xSemaphoreCreateMutex();
    s_ws_tx_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    s_cmd_q = xQueueCreate(8, sizeof(agent_cmd_t));
    if (s_lock == NULL || s_ws_tx_lock == NULL || s_events == NULL || s_cmd_q == NULL) {
        agent_init_teardown();
        return STATUS_NO_MEM;
    }

    if (xTaskCreateWithCaps(agent_worker_task, "pet_agent", AGENT_TASK_STACK_BYTES, NULL, AGENT_TASK_PRIORITY, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        LOG_ERROR("agent: worker create failed");
        agent_init_teardown();
        return STATUS_FAIL;
    }

    agent_fill_ids();
    agent_set_state(DESKTOP_PET_AGENT_STATE_IDLE);
    s_inited = true;
    LOG_INFO("agent: init uri=%s", CONFIG_DESKTOP_PET_AGENT_WS_URI);
    return STATUS_OK;
}

status_t desktop_pet_agent_session_toggle(void)
{
    return agent_post_cmd(AGENT_CMD_SESSION_TOGGLE);
}

status_t desktop_pet_agent_session_open(void)
{
    return agent_post_cmd(AGENT_CMD_SESSION_OPEN);
}

status_t desktop_pet_agent_session_close(void)
{
    s_listen_want = false;
    return agent_post_cmd(AGENT_CMD_SESSION_CLOSE);
}

status_t desktop_pet_agent_listen_start(void)
{
    s_listen_want = true;
    return agent_post_cmd(AGENT_CMD_LISTEN_START);
}

status_t desktop_pet_agent_listen_stop(void)
{
    s_listen_want = false;
    return agent_post_cmd(AGENT_CMD_LISTEN_STOP);
}

status_t desktop_pet_agent_listen_toggle(void)
{
    if (s_state == DESKTOP_PET_AGENT_STATE_LISTENING || s_listen_want) {
        return desktop_pet_agent_listen_stop();
    }
    return desktop_pet_agent_listen_start();
}

desktop_pet_agent_state_t desktop_pet_agent_get_state(void)
{
    return s_state;
}

bool desktop_pet_agent_is_open(void)
{
    return s_state == DESKTOP_PET_AGENT_STATE_OPEN || s_state == DESKTOP_PET_AGENT_STATE_LISTENING ||
           s_state == DESKTOP_PET_AGENT_STATE_SPEAKING;
}

bool desktop_pet_agent_is_listen_active(void)
{
    return s_state == DESKTOP_PET_AGENT_STATE_LISTENING || s_listen_want;
}

status_t desktop_pet_agent_copy_session_id(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0U) {
        return STATUS_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        buf[0] = '\0';
        return STATUS_TIMEOUT;
    }
    strncpy(buf, s_session_id, buf_len - 1U);
    buf[buf_len - 1U] = '\0';
    xSemaphoreGive(s_lock);
    return STATUS_OK;
}

void desktop_pet_agent_set_ui_cb(desktop_pet_agent_ui_cb_t cb)
{
    s_ui_cb = cb;
}

#else /* !CONFIG_DESKTOP_PET_AGENT_ENABLE */

status_t desktop_pet_agent_init(void)
{
    return STATUS_OK;
}

status_t desktop_pet_agent_session_toggle(void)
{
    return STATUS_NOT_SUPPORTED;
}

status_t desktop_pet_agent_session_open(void)
{
    return STATUS_NOT_SUPPORTED;
}

status_t desktop_pet_agent_session_close(void)
{
    return STATUS_NOT_SUPPORTED;
}

status_t desktop_pet_agent_listen_start(void)
{
    return STATUS_NOT_SUPPORTED;
}

status_t desktop_pet_agent_listen_stop(void)
{
    return STATUS_NOT_SUPPORTED;
}

status_t desktop_pet_agent_listen_toggle(void)
{
    return STATUS_NOT_SUPPORTED;
}

desktop_pet_agent_state_t desktop_pet_agent_get_state(void)
{
    return DESKTOP_PET_AGENT_STATE_IDLE;
}

bool desktop_pet_agent_is_open(void)
{
    return false;
}

bool desktop_pet_agent_is_listen_active(void)
{
    return false;
}

status_t desktop_pet_agent_copy_session_id(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0U) {
        return STATUS_INVALID_ARG;
    }
    buf[0] = '\0';
    return STATUS_OK;
}

void desktop_pet_agent_set_ui_cb(desktop_pet_agent_ui_cb_t cb)
{
    (void)cb;
}

#endif /* CONFIG_DESKTOP_PET_AGENT_ENABLE */
