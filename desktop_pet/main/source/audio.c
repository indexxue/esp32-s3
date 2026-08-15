/**
 * @file audio.c
 * @brief ES8311 捕获/播放 + I2S：RX 写入 PSRAM，TX 回放；写满即停。
 */

#include "audio.h"

#include "board.h"
#include "es8311.h"
#include "gpio.h"
#include "i2c.h"
#include "log.h"
#include "sdcard.h"

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define AUDIO_I2S_NUM (I2S_NUM_0)
#define AUDIO_TASK_STACK (4096U)
#define AUDIO_TASK_PRIO (5U)
#define AUDIO_CHUNK_SAMPLES (256U)
/**
 * MIC PGA：0=0dB … 7=42dB。偏高会吵，过低人声小；30dB 折中。
 */
#define AUDIO_MIC_GAIN_STEPS (5U) /* 30dB */
#define AUDIO_MIC_GAIN_REG ((uint8_t)(0x20U | (AUDIO_MIC_GAIN_STEPS & 0x07U)))
/** ADC 数字音量：0xBF≈0dB。 */
#define AUDIO_ADC_VOLUME_REG (0xBFU)
/** DAC 数字音量：0xBF≈0dB。 */
#define AUDIO_DAC_VOLUME_REG (0xBFU)
/** 仅去直流：R≈0.995 → fc≈12Hz @16kHz（不做噪声门，避免卡断/爆音）。 */
#define AUDIO_HPF_R_Q15 (32604)
#define AUDIO_SD_DIR BOARD_SDCARD_MOUNT_POINT "/record"
/** Care SFX / file play: max PCM payload @ 16k mono 16-bit */
#define AUDIO_WAV_MAX_DATA_BYTES ((size_t)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ * 2U * 8U)
#define AUDIO_WAV_PATH_MAX (200U)

static es8311_t s_codec;
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static int16_t *s_pcm;
static size_t s_pcm_cap_samples;
static size_t s_pcm_len_samples;
static bool s_ready;
static volatile bool s_recording;
static volatile bool s_playing;
static volatile bool s_paused;
static volatile bool s_streaming;
static volatile bool s_playouting;
static int s_stream_use_right; /* -1 unknown, 0 L, 1 R */
static uint64_t s_stream_energy_l;
static uint64_t s_stream_energy_r;
static size_t s_play_pos_samples;
static TaskHandle_t s_rec_task;
static TaskHandle_t s_play_task;
static TaskHandle_t s_wav_task;
static SemaphoreHandle_t s_lock;
static uint16_t s_rec_file_seq;
static int32_t s_hpf_x1;
static int32_t s_hpf_y1;
static volatile bool s_wav_playing;
static char s_wav_path[AUDIO_WAV_PATH_MAX];
static uint32_t s_wav_data_off;
static uint32_t s_wav_data_bytes;

/*
 * 板丝印 I2S_DIN/DOUT 按 Codec 脚命名时：
 *   I2S_DIN  = 进 Codec（接 ESP DOUT）
 *   I2S_DOUT = 出 Codec（接 ESP DIN）
 * 若 peak 仍为 0，把下面改成 0 再试（按 MCU 脚命名）。
 */
#ifndef AUDIO_I2S_PINS_CODEC_NAMED
#define AUDIO_I2S_PINS_CODEC_NAMED 1
#endif

static int audio_i2c_write(uint8_t addr7, const uint8_t *data, uint16_t len)
{
    if (I2cWrite((s32_t)BOARD_DESKTOP_PET_ES8311_I2C_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int audio_i2c_write_read(uint8_t addr7,
                                const uint8_t *write_data,
                                uint16_t write_len,
                                uint8_t *read_data,
                                uint16_t read_len)
{
    if (I2cWriteRead((s32_t)BOARD_DESKTOP_PET_ES8311_I2C_PORT,
                     (u16_t)addr7,
                     write_data,
                     (usize_t)write_len,
                     read_data,
                     (usize_t)read_len) != TRUE) {
        return -1;
    }
    return 0;
}

static void audio_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms == 0U ? 1U : ms));
}

static status_t audio_pa_init_off(void)
{
    GpioPinConfig_t cfg = {0};

    cfg.pin = (s32_t)BOARD_DESKTOP_PET_PA_EN_PIN;
    cfg.mode = (s32_t)GPIO_MODE_OUTPUT_E;
    cfg.pullUpEn = (s32_t)GPIO_PULL_DISABLE_E;
    cfg.pullDownEn = (s32_t)GPIO_PULL_DISABLE_E;
    cfg.intrType = (s32_t)GPIO_INTR_DISABLE_E;
    if (GpioConfigurePin(&cfg) != TRUE) {
        return STATUS_FAIL;
    }
    (void)GpioWritePin((s32_t)BOARD_DESKTOP_PET_PA_EN_PIN, 0U);
    return STATUS_OK;
}

static void audio_pa_set(bool on)
{
    u32_t level = on ? (u32_t)BOARD_DESKTOP_PET_PA_EN_ACTIVE_LEVEL : 0U;

    (void)GpioWritePin((s32_t)BOARD_DESKTOP_PET_PA_EN_PIN, level);
}

/**
 * 全双工口：停采后若 RX 仍开着却无人读，DMA 溢出会拖死控制器（TX 写返回 OK 但无声）。
 * 播放时关 RX；采音时开 RX。用标志避免重复 enable/disable（IDF 会打 E 日志）。
 */
static bool s_i2s_rx_on;
static bool s_i2s_tx_on;

static void audio_i2s_rx_set(bool on)
{
    esp_err_t err;

    if (s_i2s_rx == NULL || s_i2s_rx_on == on) {
        return;
    }
    if (on) {
        err = i2s_channel_enable(s_i2s_rx);
    } else {
        err = i2s_channel_disable(s_i2s_rx);
    }
    if (err == ESP_OK) {
        s_i2s_rx_on = on;
    } else if (err == ESP_ERR_INVALID_STATE) {
        /* 与硬件不同步时以目标状态为准，避免卡死。 */
        s_i2s_rx_on = on;
    } else {
        LOG_WARN("audio: rx %s failed: %s", on ? "enable" : "disable", esp_err_to_name(err));
    }
}

static void audio_i2s_tx_ensure(void)
{
    esp_err_t err;

    if (s_i2s_tx == NULL || s_i2s_tx_on) {
        return;
    }
    err = i2s_channel_enable(s_i2s_tx);
    if (err == ESP_OK) {
        s_i2s_tx_on = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_i2s_tx_on = true;
    } else {
        LOG_WARN("audio: tx enable failed: %s", esp_err_to_name(err));
    }
}

static void audio_i2s_prepare_playback(void)
{
    /* 只关 RX，不要动 TX：关掉 TX 会断 MCLK，听写后的 TTS 容易假写无声。 */
    audio_i2s_rx_set(false);
    audio_i2s_tx_ensure();
}

static void audio_i2s_prepare_capture(void)
{
    /* 多轮听↔说后强制 RX 复位，清 DMA 残留，减轻 read_fail / 假断连。 */
    audio_i2s_rx_set(false);
    audio_i2s_tx_ensure();
    vTaskDelay(pdMS_TO_TICKS(5));
    audio_i2s_rx_set(true);
}

static void audio_wait_task_end(TaskHandle_t *task)
{
    int i;

    for (i = 0; i < 50 && (*task != NULL); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static status_t audio_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    gpio_num_t esp_dout;
    gpio_num_t esp_din;
    i2s_std_config_t std_cfg;

#if AUDIO_I2S_PINS_CODEC_NAMED
    /* 丝印=Codec：板 DIN→ESP dout，板 DOUT→ESP din */
    esp_dout = (gpio_num_t)BOARD_DESKTOP_PET_I2S_DIN_PIN;
    esp_din = (gpio_num_t)BOARD_DESKTOP_PET_I2S_DOUT_PIN;
#else
    esp_dout = (gpio_num_t)BOARD_DESKTOP_PET_I2S_DOUT_PIN;
    esp_din = (gpio_num_t)BOARD_DESKTOP_PET_I2S_DIN_PIN;
#endif

    /* 与 IDF i2s_es8311 一致：全双工 + STEREO + MCLK×256 */
    std_cfg = (i2s_std_config_t){
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = BOARD_DESKTOP_PET_I2S_MCLK_PIN,
                .bclk = BOARD_DESKTOP_PET_I2S_BCLK_PIN,
                .ws = BOARD_DESKTOP_PET_I2S_WS_PIN,
                .dout = esp_dout,
                .din = esp_din,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    if (i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx) != ESP_OK) {
        LOG_ERROR("audio: i2s_new_channel failed");
        return STATUS_FAIL;
    }
    if (i2s_channel_init_std_mode(s_i2s_tx, &std_cfg) != ESP_OK) {
        LOG_ERROR("audio: i2s TX init_std_mode failed");
        return STATUS_FAIL;
    }
    if (i2s_channel_init_std_mode(s_i2s_rx, &std_cfg) != ESP_OK) {
        LOG_ERROR("audio: i2s RX init_std_mode failed");
        return STATUS_FAIL;
    }
    LOG_INFO("audio: I2S duplex ESP dout=GPIO%d din=GPIO%d (codec_named=%d)",
             (int)esp_dout,
             (int)esp_din,
             AUDIO_I2S_PINS_CODEC_NAMED);
    return STATUS_OK;
}

/** 一阶 DC blocker（轻柔，不做门限静音）。 */
static int16_t audio_dc_block(int16_t x)
{
    int32_t x0 = (int32_t)x;
    int32_t y;

    y = x0 - s_hpf_x1 + ((s_hpf_y1 * (int32_t)AUDIO_HPF_R_Q15) >> 15);
    s_hpf_x1 = x0;
    if (y > 32767) {
        y = 32767;
    } else if (y < -32768) {
        y = -32768;
    }
    s_hpf_y1 = y;
    return (int16_t)y;
}

static void audio_rec_task(void *arg)
{
    int16_t chunk_st[AUDIO_CHUNK_SAMPLES * 2U];
    int32_t peak_l = 0;
    int32_t peak_r = 0;
    int32_t peak_out = 0;
    uint64_t energy_l = 0U;
    uint64_t energy_r = 0U;
    /* 丢掉使能毛刺（约 40ms）。 */
    size_t skip_frames =
        ((size_t)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ * 40U) / 1000U;
    /* 预热后再锁声道，避免逐帧 L/R 切换造成爆音。 */
    size_t pick_frames =
        ((size_t)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ * 80U) / 1000U;
    int use_right = -1;

    (void)arg;
    s_hpf_x1 = 0;
    s_hpf_y1 = 0;

    while (s_recording) {
        size_t nbytes = 0U;
        size_t room;
        size_t want;
        size_t frames;
        size_t i;
        size_t stored;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        room = (s_pcm_cap_samples > s_pcm_len_samples) ? (s_pcm_cap_samples - s_pcm_len_samples) : 0U;
        xSemaphoreGive(s_lock);

        if (room == 0U) {
            LOG_INFO("audio: pcm buffer full, auto-stop");
            s_recording = false;
            (void)es8311_stop(&s_codec);
            break;
        }

        want = (room < AUDIO_CHUNK_SAMPLES) ? room : AUDIO_CHUNK_SAMPLES;
        if (i2s_channel_read(s_i2s_rx, chunk_st, want * 2U * sizeof(int16_t), &nbytes, pdMS_TO_TICKS(200)) !=
            ESP_OK) {
            continue;
        }
        frames = nbytes / (sizeof(int16_t) * 2U);
        if (frames == 0U) {
            continue;
        }

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        if ((s_pcm == NULL) || (s_pcm_len_samples >= s_pcm_cap_samples)) {
            xSemaphoreGive(s_lock);
            continue;
        }

        stored = 0U;
        for (i = 0U; i < frames; i++) {
            const int16_t l = chunk_st[i * 2U];
            const int16_t r = chunk_st[i * 2U + 1U];
            const int32_t al = (l < 0) ? -(int32_t)l : (int32_t)l;
            const int32_t ar = (r < 0) ? -(int32_t)r : (int32_t)r;
            int16_t sample;
            int16_t processed;
            int32_t ap;

            if (al > peak_l) {
                peak_l = al;
            }
            if (ar > peak_r) {
                peak_r = ar;
            }

            if (skip_frames > 0U) {
                skip_frames--;
                continue;
            }

            energy_l += (uint64_t)al;
            energy_r += (uint64_t)ar;

            if (use_right < 0) {
                if (pick_frames > 0U) {
                    pick_frames--;
                    continue;
                }
                use_right = (energy_r > energy_l) ? 1 : 0;
                LOG_INFO("audio: lock channel %s", use_right ? "R" : "L");
            }

            sample = (use_right != 0) ? r : l;
            processed = audio_dc_block(sample);
            ap = (processed < 0) ? -(int32_t)processed : (int32_t)processed;
            if (ap > peak_out) {
                peak_out = ap;
            }

            if ((s_pcm_len_samples + stored) >= s_pcm_cap_samples) {
                break;
            }
            s_pcm[s_pcm_len_samples + stored] = processed;
            stored++;
        }
        s_pcm_len_samples += stored;
        xSemaphoreGive(s_lock);
    }

    LOG_INFO("audio: rec peak L=%ld R=%ld out=%ld energy L=%llu R=%llu",
             (long)peak_l,
             (long)peak_r,
             (long)peak_out,
             (unsigned long long)energy_l,
             (unsigned long long)energy_r);
    s_rec_task = NULL;
    vTaskDelete(NULL);
}

static void audio_play_task(void *arg)
{
    int16_t chunk_st[AUDIO_CHUNK_SAMPLES * 2U];

    (void)arg;

    while (s_playing) {
        size_t remain;
        size_t want;
        size_t i;
        size_t nbytes = 0U;

        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        if ((s_pcm == NULL) || (s_play_pos_samples >= s_pcm_len_samples)) {
            xSemaphoreGive(s_lock);
            break;
        }
        remain = s_pcm_len_samples - s_play_pos_samples;
        want = (remain < AUDIO_CHUNK_SAMPLES) ? remain : AUDIO_CHUNK_SAMPLES;
        for (i = 0U; i < want; i++) {
            const int16_t sample = s_pcm[s_play_pos_samples + i];

            chunk_st[i * 2U] = sample;
            chunk_st[i * 2U + 1U] = sample;
        }
        s_play_pos_samples += want;
        xSemaphoreGive(s_lock);

        (void)i2s_channel_write(s_i2s_tx, chunk_st, want * 2U * sizeof(int16_t), &nbytes,
                                pdMS_TO_TICKS(200));
    }

    audio_pa_set(false);
    (void)es8311_stop(&s_codec);
    s_playing = false;
    s_paused = false;
    s_play_task = NULL;
    LOG_INFO("audio: play end, pos=%u", (unsigned)s_play_pos_samples);
    vTaskDelete(NULL);
}

status_t desktop_pet_audio_init(void)
{
#if !DESKTOP_PET_ENABLE_AUDIO
    LOG_INFO("audio disabled (DESKTOP_PET_ENABLE_AUDIO=0)");
    return STATUS_FAIL;
#else
    I2cDeviceConfig_t icfg = {0};
    es8311_config_t ccfg = {0};
    size_t bytes;

    if (s_ready) {
        return STATUS_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return STATUS_FAIL;
        }
    }

    if (audio_pa_init_off() != STATUS_OK) {
        LOG_WARN("audio: PA_EN gpio init failed");
    }

    icfg.port = (s32_t)BOARD_DESKTOP_PET_ES8311_I2C_PORT;
    icfg.deviceAddress7bit = (u16_t)BOARD_DESKTOP_PET_ES8311_I2C_ADDR;
    icfg.clockSpeedHz = 0U;
    icfg.transactionTimeoutMs = 0U;
    if (I2cRegisterDevice(&icfg) != TRUE) {
        LOG_ERROR("audio: ES8311 I2cRegisterDevice failed");
        return STATUS_FAIL;
    }

    if (I2cProbe((s32_t)BOARD_DESKTOP_PET_ES8311_I2C_PORT, (u16_t)BOARD_DESKTOP_PET_ES8311_I2C_ADDR) != TRUE) {
        LOG_ERROR("audio: ES8311 probe fail @0x%02X", (unsigned)BOARD_DESKTOP_PET_ES8311_I2C_ADDR);
        return STATUS_FAIL;
    }
    LOG_INFO("audio: ES8311 probed @0x%02X", (unsigned)BOARD_DESKTOP_PET_ES8311_I2C_ADDR);

    /* 先起 I2S 全双工（含 MCLK），再配 Codec；时钟常开，避免启停丢锁。 */
    if (audio_i2s_init() != STATUS_OK) {
        return STATUS_FAIL;
    }
    if (i2s_channel_enable(s_i2s_tx) != ESP_OK) {
        LOG_ERROR("audio: i2s TX enable failed");
        return STATUS_FAIL;
    }
    s_i2s_tx_on = true;
    if (i2s_channel_enable(s_i2s_rx) != ESP_OK) {
        LOG_ERROR("audio: i2s RX enable failed");
        return STATUS_FAIL;
    }
    s_i2s_rx_on = true;
    vTaskDelay(pdMS_TO_TICKS(20));
    /* 默认关 RX：空闲无人读时溢出会拖死全双工；采音时再开。 */
    audio_i2s_rx_set(false);

    ccfg.write = audio_i2c_write;
    ccfg.write_read = audio_i2c_write_read;
    ccfg.delay_ms = audio_delay_ms;
    ccfg.i2c_addr7 = (uint8_t)BOARD_DESKTOP_PET_ES8311_I2C_ADDR;
    ccfg.sample_rate_hz = DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ;
    ccfg.i2s_port = (int)AUDIO_I2S_NUM;
    ccfg.mclk_div = 256U;
    ccfg.use_mclk = true;

    {
        es8311_status_t cst = es8311_init_with_config(&s_codec, &ccfg);

        if (cst != ES8311_OK) {
            LOG_ERROR("audio: es8311_init_with_config failed st=%d", (int)cst);
            return STATUS_FAIL;
        }
    }

    s_pcm_cap_samples = (size_t)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ * (size_t)DESKTOP_PET_AUDIO_MAX_SECONDS;
    bytes = s_pcm_cap_samples * sizeof(int16_t);
    if (s_pcm == NULL) {
        s_pcm = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pcm == NULL) {
            s_pcm = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (s_pcm == NULL) {
        LOG_ERROR("audio: pcm buffer alloc failed (%u bytes)", (unsigned)bytes);
        return STATUS_FAIL;
    }
    s_pcm_len_samples = 0U;

    s_ready = true;
    LOG_INFO("audio ready: ES8311+I2S duplex %uHz mono, buf=%us, clk=MCLK",
             (unsigned)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ,
             (unsigned)DESKTOP_PET_AUDIO_MAX_SECONDS);
    return STATUS_OK;
#endif
}

bool desktop_pet_audio_is_ready(void)
{
    return s_ready;
}

bool desktop_pet_audio_is_recording(void)
{
    return s_recording;
}

bool desktop_pet_audio_is_playing(void)
{
    return s_playing || s_wav_playing;
}

bool desktop_pet_audio_is_paused(void)
{
    return s_playing && s_paused;
}

static void audio_es8311_dump_regs(void)
{
    /* 捕获通路关键：时钟/SDP/模拟电源/MIC 选择/PGA/ADC 音量 */
    static const uint8_t regs[] = {
        0x00U, 0x01U, 0x02U, 0x0AU, 0x0DU, 0x0EU, 0x14U, 0x15U, 0x16U, 0x17U,
    };
    uint8_t vals[sizeof(regs)];
    size_t i;

    for (i = 0U; i < sizeof(regs); i++) {
        if (es8311_read_reg(&s_codec, regs[i], &vals[i]) != ES8311_OK) {
            LOG_WARN("audio: es8311 dump fail @0x%02X", (unsigned)regs[i]);
            return;
        }
    }
    LOG_INFO("audio: es8311 dump "
             "00=%02X 01=%02X 02=%02X 0A=%02X 0D=%02X 0E=%02X "
             "14=%02X 15=%02X 16=%02X 17=%02X",
             (unsigned)vals[0],
             (unsigned)vals[1],
             (unsigned)vals[2],
             (unsigned)vals[3],
             (unsigned)vals[4],
             (unsigned)vals[5],
             (unsigned)vals[6],
             (unsigned)vals[7],
             (unsigned)vals[8],
             (unsigned)vals[9]);
}

status_t desktop_pet_audio_record_start(void)
{
    int16_t drain[256];
    int d;

    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (s_recording) {
        return STATUS_OK;
    }
    if (s_streaming) {
        LOG_WARN("audio: record blocked, streaming");
        return STATUS_INVALID_STATE;
    }
    if (s_playouting) {
        LOG_WARN("audio: record blocked, playouting (wait for TTS)");
        return STATUS_INVALID_STATE;
    }
    if (desktop_pet_audio_play_stop() != STATUS_OK) {
        LOG_WARN("audio: play_stop before record failed");
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return STATUS_FAIL;
    }
    s_pcm_len_samples = 0U;
    xSemaphoreGive(s_lock);

    /* I2S/MCLK 已在 init 常开；这里只起 Codec ADC。 */
    if (es8311_set_mode(&s_codec, ES8311_MODE_CAPTURE) != ES8311_OK) {
        return STATUS_FAIL;
    }
    if (es8311_start(&s_codec) != ES8311_OK) {
        return STATUS_FAIL;
    }
    (void)es8311_set_mic_gain(&s_codec, AUDIO_MIC_GAIN_STEPS);
    (void)es8311_set_adc_volume(&s_codec, AUDIO_ADC_VOLUME_REG);
    audio_i2s_prepare_capture();
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_es8311_dump_regs();

    for (d = 0; d < 6; d++) {
        size_t nbytes = 0U;
        (void)i2s_channel_read(s_i2s_rx, drain, sizeof(drain), &nbytes, pdMS_TO_TICKS(50));
    }
    LOG_INFO("audio: raw after drain L=%d R=%d L2=%d R2=%d",
             (int)drain[0],
             (int)drain[1],
             (int)drain[2],
             (int)drain[3]);

    LOG_INFO("audio: micgain_reg=0x%02X adc_vol=0x%02X",
             (unsigned)AUDIO_MIC_GAIN_REG,
             (unsigned)AUDIO_ADC_VOLUME_REG);

    s_recording = true;
    if (xTaskCreate(audio_rec_task, "pet_rec", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO, &s_rec_task) != pdPASS) {
        s_recording = false;
        (void)es8311_stop(&s_codec);
        LOG_ERROR("audio: rec task create failed");
        return STATUS_FAIL;
    }

    LOG_INFO("audio: record start");
    return STATUS_OK;
}

status_t desktop_pet_audio_record_stop(void)
{
    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (!s_recording) {
        return STATUS_OK;
    }

    s_recording = false;
    audio_wait_task_end(&s_rec_task);

    (void)es8311_stop(&s_codec);
    audio_i2s_rx_set(false);
    /* TX 保持使能，维持 MCLK。 */

    LOG_INFO("audio: record stop, samples=%u (%.2fs)",
             (unsigned)s_pcm_len_samples,
             (double)s_pcm_len_samples / (double)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ);
    return STATUS_OK;
}

status_t desktop_pet_audio_play_stop(void)
{
    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (s_wav_playing || (s_wav_task != NULL)) {
        s_wav_playing = false;
        audio_wait_task_end(&s_wav_task);
        audio_pa_set(false);
        (void)es8311_stop(&s_codec);
        LOG_INFO("audio: wav play stop");
    }
    if (!s_playing && (s_play_task == NULL)) {
        return STATUS_OK;
    }

    s_playing = false;
    s_paused = false;
    audio_wait_task_end(&s_play_task);
    audio_pa_set(false);
    (void)es8311_stop(&s_codec);
    s_play_pos_samples = 0U;
    LOG_INFO("audio: play stop");
    return STATUS_OK;
}

status_t desktop_pet_audio_play_pause(void)
{
    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (!s_playing) {
        return STATUS_OK;
    }

    s_paused = true;
    audio_pa_set(false);
    LOG_INFO("audio: play pause, pos=%u", (unsigned)s_play_pos_samples);
    return STATUS_OK;
}

status_t desktop_pet_audio_play_start(void)
{
    size_t pcm_len;

    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (s_recording) {
        LOG_WARN("audio: play ignored, recording");
        return STATUS_FAIL;
    }
    if (s_streaming || s_playouting || s_wav_playing) {
        LOG_WARN("audio: play ignored, stream=%d playout=%d wav=%d", (int)s_streaming,
                 (int)s_playouting, (int)s_wav_playing);
        return STATUS_FAIL;
    }

    if (s_playing && s_paused) {
        audio_pa_set(true);
        s_paused = false;
        LOG_INFO("audio: play resume, pos=%u", (unsigned)s_play_pos_samples);
        return STATUS_OK;
    }
    if (s_playing) {
        return STATUS_OK;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return STATUS_FAIL;
    }
    pcm_len = s_pcm_len_samples;
    xSemaphoreGive(s_lock);
    if ((s_pcm == NULL) || (pcm_len == 0U)) {
        LOG_WARN("audio: no pcm to play");
        return STATUS_FAIL;
    }

    s_play_pos_samples = 0U;
    s_paused = false;

    audio_i2s_prepare_playback();

    if (es8311_set_mode(&s_codec, ES8311_MODE_PLAYBACK) != ES8311_OK) {
        return STATUS_FAIL;
    }
    if (es8311_start(&s_codec) != ES8311_OK) {
        return STATUS_FAIL;
    }
    (void)es8311_set_dac_volume(&s_codec, AUDIO_DAC_VOLUME_REG);
    audio_pa_set(true);
    vTaskDelay(pdMS_TO_TICKS(30));

    s_playing = true;
    if (xTaskCreate(audio_play_task, "pet_play", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO, &s_play_task) !=
        pdPASS) {
        s_playing = false;
        audio_pa_set(false);
        (void)es8311_stop(&s_codec);
        LOG_ERROR("audio: play task create failed");
        return STATUS_FAIL;
    }

    LOG_INFO("audio: play start, samples=%u", (unsigned)pcm_len);
    return STATUS_OK;
}

size_t desktop_pet_audio_pcm_bytes(void)
{
    return s_pcm_len_samples * sizeof(int16_t);
}

const int16_t *desktop_pet_audio_pcm_data(void)
{
    return s_pcm;
}

status_t desktop_pet_audio_save_to_sd(char *out_path, size_t out_path_len)
{
    char path[64];
    FILE *fp;
    const int16_t *pcm;
    size_t pcm_bytes;
    uint32_t data_bytes;
    uint32_t byte_rate;
    uint16_t block_align;
    uint8_t hdr[44];
    size_t nw;

    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (sdcard_get_card() == NULL) {
        LOG_WARN("audio: SD not mounted, skip save");
        return STATUS_FAIL;
    }

    pcm = desktop_pet_audio_pcm_data();
    pcm_bytes = desktop_pet_audio_pcm_bytes();
    if ((pcm == NULL) || (pcm_bytes == 0U)) {
        LOG_WARN("audio: no pcm to save");
        return STATUS_FAIL;
    }

    if (mkdir(AUDIO_SD_DIR, 0775) != 0) {
        if (errno != EEXIST) {
            LOG_ERROR("audio: mkdir %s failed errno=%d", AUDIO_SD_DIR, errno);
            return STATUS_FAIL;
        }
    }

    s_rec_file_seq++;
    if (s_rec_file_seq == 0U) {
        s_rec_file_seq = 1U;
    }
    (void)snprintf(path, sizeof(path), AUDIO_SD_DIR "/rec_%04u.wav", (unsigned)s_rec_file_seq);

    fp = fopen(path, "wb");
    if (fp == NULL) {
        LOG_ERROR("audio: fopen %s failed errno=%d", path, errno);
        return STATUS_FAIL;
    }

    data_bytes = (uint32_t)pcm_bytes;
    block_align = (uint16_t)((DESKTOP_PET_AUDIO_CHANNELS * DESKTOP_PET_AUDIO_BITS) / 8U);
    byte_rate = DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ * (uint32_t)block_align;

    /* RIFF WAV PCM header (little-endian) */
    memcpy(&hdr[0], "RIFF", 4);
    {
        uint32_t riff_size = 36U + data_bytes;
        hdr[4] = (uint8_t)(riff_size);
        hdr[5] = (uint8_t)(riff_size >> 8);
        hdr[6] = (uint8_t)(riff_size >> 16);
        hdr[7] = (uint8_t)(riff_size >> 24);
    }
    memcpy(&hdr[8], "WAVE", 4);
    memcpy(&hdr[12], "fmt ", 4);
    hdr[16] = 16;
    hdr[17] = 0;
    hdr[18] = 0;
    hdr[19] = 0; /* fmt chunk size */
    hdr[20] = 1;
    hdr[21] = 0; /* PCM */
    hdr[22] = (uint8_t)DESKTOP_PET_AUDIO_CHANNELS;
    hdr[23] = 0;
    {
        uint32_t rate = DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ;
        hdr[24] = (uint8_t)(rate);
        hdr[25] = (uint8_t)(rate >> 8);
        hdr[26] = (uint8_t)(rate >> 16);
        hdr[27] = (uint8_t)(rate >> 24);
        hdr[28] = (uint8_t)(byte_rate);
        hdr[29] = (uint8_t)(byte_rate >> 8);
        hdr[30] = (uint8_t)(byte_rate >> 16);
        hdr[31] = (uint8_t)(byte_rate >> 24);
    }
    hdr[32] = (uint8_t)(block_align);
    hdr[33] = (uint8_t)(block_align >> 8);
    hdr[34] = (uint8_t)DESKTOP_PET_AUDIO_BITS;
    hdr[35] = 0;
    memcpy(&hdr[36], "data", 4);
    hdr[40] = (uint8_t)(data_bytes);
    hdr[41] = (uint8_t)(data_bytes >> 8);
    hdr[42] = (uint8_t)(data_bytes >> 16);
    hdr[43] = (uint8_t)(data_bytes >> 24);

    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        LOG_ERROR("audio: write wav hdr failed");
        fclose(fp);
        return STATUS_FAIL;
    }
    nw = fwrite(pcm, 1, pcm_bytes, fp);
    (void)fflush(fp);
    fclose(fp);
    if (nw != pcm_bytes) {
        LOG_ERROR("audio: write pcm short %u/%u", (unsigned)nw, (unsigned)pcm_bytes);
        return STATUS_FAIL;
    }

    LOG_INFO("audio: saved %s (%u bytes pcm)", path, (unsigned)pcm_bytes);
    if ((out_path != NULL) && (out_path_len > 0U)) {
        (void)snprintf(out_path, out_path_len, "%s", path);
    }
    return STATUS_OK;
}

status_t desktop_pet_audio_stream_start(void)
{
    int16_t drain[256];
    int16_t warm[320 * 2];
    int d;
    size_t warm_need;
    size_t warm_got = 0U;

    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (s_streaming) {
        return STATUS_OK;
    }
    if (s_recording || s_playing || s_playouting) {
        LOG_WARN("audio: stream blocked, rec=%d play=%d playout=%d", (int)s_recording, (int)s_playing,
                 (int)s_playouting);
        return STATUS_INVALID_STATE;
    }

    if (es8311_set_mode(&s_codec, ES8311_MODE_CAPTURE) != ES8311_OK) {
        return STATUS_FAIL;
    }
    if (es8311_start(&s_codec) != ES8311_OK) {
        return STATUS_FAIL;
    }
    (void)es8311_set_mic_gain(&s_codec, AUDIO_MIC_GAIN_STEPS);
    (void)es8311_set_adc_volume(&s_codec, AUDIO_ADC_VOLUME_REG);
    audio_i2s_prepare_capture();
    vTaskDelay(pdMS_TO_TICKS(30));

    for (d = 0; d < 4; d++) {
        size_t nbytes = 0U;
        (void)i2s_channel_read(s_i2s_rx, drain, sizeof(drain), &nbytes, pdMS_TO_TICKS(40));
    }

    s_hpf_x1 = 0;
    s_hpf_y1 = 0;
    s_stream_use_right = -1;
    s_stream_energy_l = 0U;
    s_stream_energy_r = 0U;
    warm_need = ((size_t)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ * 80U) / 1000U;

    while (warm_got < warm_need) {
        size_t nbytes = 0U;
        size_t frames;
        size_t i;
        size_t ask = warm_need - warm_got;

        if (ask > 320U) {
            ask = 320U;
        }
        if (i2s_channel_read(s_i2s_rx, warm, ask * 2U * sizeof(int16_t), &nbytes, pdMS_TO_TICKS(200)) != ESP_OK) {
            break;
        }
        frames = nbytes / (sizeof(int16_t) * 2U);
        for (i = 0U; i < frames; i++) {
            const int16_t l = warm[i * 2U];
            const int16_t r = warm[i * 2U + 1U];
            const int32_t al = (l < 0) ? -(int32_t)l : (int32_t)l;
            const int32_t ar = (r < 0) ? -(int32_t)r : (int32_t)r;

            s_stream_energy_l += (uint64_t)al;
            s_stream_energy_r += (uint64_t)ar;
            warm_got++;
        }
    }

    s_stream_use_right = (s_stream_energy_r > s_stream_energy_l) ? 1 : 0;
    s_streaming = true;
    LOG_INFO("audio: stream start channel=%s", s_stream_use_right ? "R" : "L");
    return STATUS_OK;
}

status_t desktop_pet_audio_stream_stop(void)
{
    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (!s_streaming) {
        return STATUS_OK;
    }

    s_streaming = false;
    (void)es8311_stop(&s_codec);
    /* 停采后立刻关 RX，避免无人读导致 DMA 溢出拖死 TX。 */
    audio_i2s_rx_set(false);
    LOG_INFO("audio: stream stop");
    return STATUS_OK;
}

bool desktop_pet_audio_is_streaming(void)
{
    return s_streaming;
}

status_t desktop_pet_audio_stream_read_mono(int16_t *out, size_t samples, uint32_t timeout_ms, size_t *out_got)
{
    static int16_t s_stereo[AUDIO_CHUNK_SAMPLES * 2U];
    size_t nbytes = 0U;
    size_t frames;
    size_t i;
    size_t got = 0U;
    size_t ask = samples;
    esp_err_t err;

    if (out_got != NULL) {
        *out_got = 0U;
    }
    if (!s_ready || !s_streaming || out == NULL || samples == 0U) {
        return STATUS_INVALID_ARG;
    }
    /* 与 debug Rec 相同块大小，避免一次读过大导致 I2S 超时。 */
    if (ask > AUDIO_CHUNK_SAMPLES) {
        ask = AUDIO_CHUNK_SAMPLES;
    }

    err = i2s_channel_read(s_i2s_rx, s_stereo, ask * 2U * sizeof(int16_t), &nbytes,
                           pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        return STATUS_TIMEOUT;
    }
    frames = nbytes / (sizeof(int16_t) * 2U);
    if (frames > ask) {
        frames = ask;
    }

    for (i = 0U; i < frames; i++) {
        const int16_t l = s_stereo[i * 2U];
        const int16_t r = s_stereo[i * 2U + 1U];
        const int16_t sample = (s_stream_use_right != 0) ? r : l;

        out[got++] = audio_dc_block(sample);
    }

    if (out_got != NULL) {
        *out_got = got;
    }
    return (got > 0U) ? STATUS_OK : STATUS_FAIL;
}

status_t desktop_pet_audio_playout_start(void)
{
    uint8_t dac_vol = 0U;
    uint8_t sdp_in = 0U;

    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (s_playouting) {
        return STATUS_OK;
    }
    if (s_recording || s_streaming) {
        LOG_WARN("audio: playout blocked, rec=%d stream=%d", (int)s_recording, (int)s_streaming);
        return STATUS_INVALID_STATE;
    }
    /* Agent TTS wins over Care SFX / debug PCM play. */
    if (s_playing || s_wav_playing) {
        (void)desktop_pet_audio_play_stop();
    }

    audio_i2s_prepare_playback();

    /* 强制停一下再起，避免 listen 后 running 状态导致 start 空操作。 */
    (void)es8311_stop(&s_codec);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (es8311_set_mode(&s_codec, ES8311_MODE_PLAYBACK) != ES8311_OK) {
        return STATUS_FAIL;
    }
    if (es8311_start(&s_codec) != ES8311_OK) {
        return STATUS_FAIL;
    }
    (void)es8311_set_dac_volume(&s_codec, AUDIO_DAC_VOLUME_REG);
    audio_pa_set(true);
    vTaskDelay(pdMS_TO_TICKS(50));

    (void)es8311_read_reg(&s_codec, 0x32U, &dac_vol);
    (void)es8311_read_reg(&s_codec, 0x09U, &sdp_in);
    s_playouting = true;
    LOG_INFO("audio: playout start dac=0x%02X sdp09=0x%02X", (unsigned)dac_vol, (unsigned)sdp_in);
    return STATUS_OK;
}

status_t desktop_pet_audio_playout_stop(void)
{
    if (!s_ready) {
        return STATUS_FAIL;
    }
    if (!s_playouting) {
        return STATUS_OK;
    }

    s_playouting = false;
    audio_pa_set(false);
    (void)es8311_stop(&s_codec);
    LOG_INFO("audio: playout stop");
    return STATUS_OK;
}

bool desktop_pet_audio_is_playouting(void)
{
    return s_playouting;
}

status_t desktop_pet_audio_playout_write_mono(const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    static int16_t s_stereo[AUDIO_CHUNK_SAMPLES * 2U];
    static uint32_t s_write_frames;
    size_t left = samples;
    size_t off = 0U;
    int32_t peak = 0;

    if (!s_ready || !s_playouting || pcm == NULL || samples == 0U) {
        return STATUS_INVALID_ARG;
    }

    for (size_t p = 0U; p < samples; p++) {
        int32_t a = (pcm[p] < 0) ? -(int32_t)pcm[p] : (int32_t)pcm[p];

        if (a > peak) {
            peak = a;
        }
    }

    while (left > 0U) {
        size_t chunk = left;
        size_t i;
        size_t nbytes = 0U;
        size_t want;
        int retry;
        esp_err_t err = ESP_FAIL;

        if (chunk > AUDIO_CHUNK_SAMPLES) {
            chunk = AUDIO_CHUNK_SAMPLES;
        }
        want = chunk * 2U * sizeof(int16_t);
        for (i = 0U; i < chunk; i++) {
            const int16_t s = pcm[off + i];

            s_stereo[i * 2U] = s;
            s_stereo[i * 2U + 1U] = s;
        }
        for (retry = 0; retry < 3; retry++) {
            nbytes = 0U;
            err = i2s_channel_write(s_i2s_tx, s_stereo, want, &nbytes, pdMS_TO_TICKS(timeout_ms));
            if (err == ESP_OK && nbytes == want) {
                break;
            }
        }
        if (err != ESP_OK || nbytes != want) {
            LOG_WARN("audio: playout write fail err=%s nbytes=%u/%u", esp_err_to_name(err), (unsigned)nbytes,
                     (unsigned)want);
            return STATUS_TIMEOUT;
        }
        off += chunk;
        left -= chunk;
    }

    s_write_frames++;
    if (s_write_frames <= 3U || (s_write_frames % 40U) == 0U) {
        LOG_INFO("audio: playout pcm frames=%u samples=%u peak=%d", (unsigned)s_write_frames, (unsigned)samples,
                 (int)peak);
    }
    return STATUS_OK;
}

static uint32_t audio_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t audio_rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/**
 * Parse PCM WAV: require 16 kHz / mono / 16-bit. Sets s_wav_data_off / s_wav_data_bytes.
 */
static bool audio_wav_parse(FILE *fp)
{
    uint8_t hdr[12];
    uint32_t rate = 0U;
    uint16_t channels = 0U;
    uint16_t bits = 0U;
    uint16_t audio_fmt = 0U;
    bool got_fmt = false;
    long file_pos;

    s_wav_data_off = 0U;
    s_wav_data_bytes = 0U;
    if (fread(hdr, 1, 12, fp) != 12U) {
        return false;
    }
    if ((memcmp(hdr, "RIFF", 4) != 0) || (memcmp(&hdr[8], "WAVE", 4) != 0)) {
        return false;
    }
    for (;;) {
        uint8_t ch[8];
        uint32_t csize;
        long skip;

        if (fread(ch, 1, 8, fp) != 8U) {
            break;
        }
        csize = audio_rd32(&ch[4]);
        file_pos = ftell(fp);
        if (file_pos < 0) {
            return false;
        }
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];

            if (csize < 16U) {
                return false;
            }
            if (fread(fmt, 1, 16, fp) != 16U) {
                return false;
            }
            audio_fmt = audio_rd16(&fmt[0]);
            channels = audio_rd16(&fmt[2]);
            rate = audio_rd32(&fmt[4]);
            bits = audio_rd16(&fmt[14]);
            got_fmt = true;
            skip = (long)csize - 16L;
            if (skip > 0) {
                if (fseek(fp, skip, SEEK_CUR) != 0) {
                    return false;
                }
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            s_wav_data_off = (uint32_t)file_pos;
            s_wav_data_bytes = csize;
            break;
        } else {
            skip = (long)csize;
            if ((skip & 1L) != 0) {
                skip++;
            }
            if (fseek(fp, skip, SEEK_CUR) != 0) {
                return false;
            }
            continue;
        }
        if ((csize & 1U) != 0U) {
            (void)fseek(fp, 1, SEEK_CUR);
        }
    }
    if (!got_fmt || (s_wav_data_bytes == 0U) || (s_wav_data_off == 0U)) {
        return false;
    }
    if ((audio_fmt != 1U) || (channels != DESKTOP_PET_AUDIO_CHANNELS) ||
        (rate != DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ) || (bits != DESKTOP_PET_AUDIO_BITS)) {
        LOG_WARN("audio: wav fmt unsupported fmt=%u ch=%u rate=%u bits=%u", (unsigned)audio_fmt,
                 (unsigned)channels, (unsigned)rate, (unsigned)bits);
        return false;
    }
    if (s_wav_data_bytes > AUDIO_WAV_MAX_DATA_BYTES) {
        LOG_WARN("audio: wav truncate %u -> %u", (unsigned)s_wav_data_bytes,
                 (unsigned)AUDIO_WAV_MAX_DATA_BYTES);
        s_wav_data_bytes = (uint32_t)AUDIO_WAV_MAX_DATA_BYTES;
    }
    return true;
}

static void audio_wav_play_task(void *arg)
{
    FILE *fp;
    int16_t mono[AUDIO_CHUNK_SAMPLES];
    int16_t stereo[AUDIO_CHUNK_SAMPLES * 2U];
    uint32_t left;

    (void)arg;
    fp = fopen(s_wav_path, "rb");
    if ((fp == NULL) || (fseek(fp, (long)s_wav_data_off, SEEK_SET) != 0)) {
        if (fp != NULL) {
            (void)fclose(fp);
        }
        LOG_WARN("audio: wav open/seek fail %s", s_wav_path);
        s_wav_playing = false;
        audio_pa_set(false);
        (void)es8311_stop(&s_codec);
        s_wav_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    left = s_wav_data_bytes;
    while (s_wav_playing && (left > 0U)) {
        size_t want_bytes =
            (left < (AUDIO_CHUNK_SAMPLES * 2U)) ? (size_t)left : (AUDIO_CHUNK_SAMPLES * 2U);
        size_t nread;
        size_t samples;
        size_t i;
        size_t nbytes = 0U;

        nread = fread(mono, 1, want_bytes, fp);
        if (nread < 2U) {
            break;
        }
        nread &= ~(size_t)1U;
        samples = nread / 2U;
        for (i = 0U; i < samples; i++) {
            stereo[i * 2U] = mono[i];
            stereo[i * 2U + 1U] = mono[i];
        }
        (void)i2s_channel_write(s_i2s_tx, stereo, samples * 2U * sizeof(int16_t), &nbytes,
                                pdMS_TO_TICKS(200));
        if (left >= (uint32_t)nread) {
            left -= (uint32_t)nread;
        } else {
            left = 0U;
        }
    }

    (void)fclose(fp);
    audio_pa_set(false);
    (void)es8311_stop(&s_codec);
    s_wav_playing = false;
    s_wav_task = NULL;
    LOG_INFO("audio: wav play end");
    vTaskDelete(NULL);
}

status_t desktop_pet_audio_play_wav_path(const char *abs_path)
{
    FILE *fp;
    size_t n;

    if (!s_ready || (abs_path == NULL) || (abs_path[0] == '\0')) {
        return STATUS_INVALID_ARG;
    }
    if (s_recording || s_streaming || s_playouting) {
        LOG_WARN("audio: wav ignored, rec=%d stream=%d playout=%d", (int)s_recording, (int)s_streaming,
                 (int)s_playouting);
        return STATUS_FAIL;
    }

    n = strlen(abs_path);
    if ((n == 0U) || (n >= AUDIO_WAV_PATH_MAX)) {
        return STATUS_INVALID_ARG;
    }

    (void)desktop_pet_audio_play_stop();

    fp = fopen(abs_path, "rb");
    if (fp == NULL) {
        LOG_WARN("audio: wav fopen fail %s errno=%d", abs_path, errno);
        return STATUS_FAIL;
    }
    if (!audio_wav_parse(fp)) {
        (void)fclose(fp);
        LOG_WARN("audio: wav parse fail %s", abs_path);
        return STATUS_FAIL;
    }
    (void)fclose(fp);

    (void)memcpy(s_wav_path, abs_path, n + 1U);
    audio_i2s_prepare_playback();
    if (es8311_set_mode(&s_codec, ES8311_MODE_PLAYBACK) != ES8311_OK) {
        return STATUS_FAIL;
    }
    if (es8311_start(&s_codec) != ES8311_OK) {
        return STATUS_FAIL;
    }
    (void)es8311_set_dac_volume(&s_codec, AUDIO_DAC_VOLUME_REG);
    audio_pa_set(true);
    s_wav_playing = true;
    if (xTaskCreate(audio_wav_play_task, "pet_wav", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO,
                    &s_wav_task) != pdPASS) {
        s_wav_playing = false;
        audio_pa_set(false);
        (void)es8311_stop(&s_codec);
        LOG_ERROR("audio: wav task create failed");
        return STATUS_FAIL;
    }
    LOG_INFO("audio: wav play %s (%u B)", abs_path, (unsigned)s_wav_data_bytes);
    return STATUS_OK;
}
