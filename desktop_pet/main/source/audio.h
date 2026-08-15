/**
 * @file audio.h
 * @brief ES8311 + I2S record/play; PCM in PSRAM, stop writes WAV under /sdcard/record/.
 */

#ifndef AUDIO_H
#define AUDIO_H

#include "type.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ (16000U)
#define DESKTOP_PET_AUDIO_BITS (16U)
#define DESKTOP_PET_AUDIO_CHANNELS (1U)
/** 最长缓存约 8s @ 16kHz mono 16-bit */
#define DESKTOP_PET_AUDIO_MAX_SECONDS (8U)

status_t desktop_pet_audio_init(void);
bool desktop_pet_audio_is_ready(void);
bool desktop_pet_audio_is_recording(void);
bool desktop_pet_audio_is_playing(void);
bool desktop_pet_audio_is_paused(void);

status_t desktop_pet_audio_record_start(void);
status_t desktop_pet_audio_record_stop(void);

/** 从当前 PCM 开头播放；若已暂停则继续。录音中或无数据时失败。 */
status_t desktop_pet_audio_play_start(void);
/** 暂停播放（保留进度）；未在播放时成功返回。 */
status_t desktop_pet_audio_play_pause(void);
/** 停止播放并回到开头。 */
status_t desktop_pet_audio_play_stop(void);

size_t desktop_pet_audio_pcm_bytes(void);
const int16_t *desktop_pet_audio_pcm_data(void);

/**
 * 将当前 PCM 写成 WAV 到 `/sdcard/record/rec_XXXX.wav`。
 * 自动建目录；无卡/未挂载/无数据时失败。
 * @param out_path 可选，写入实际路径（建议 ≥64 字节）
 */
status_t desktop_pet_audio_save_to_sd(char *out_path, size_t out_path_len);

/**
 * 流式采音（Z1-3）：与 debug Rec/Play 互斥。
 * 不写 PSRAM 大缓冲；由调用方周期性 `stream_read_mono`。
 */
status_t desktop_pet_audio_stream_start(void);
status_t desktop_pet_audio_stream_stop(void);
bool desktop_pet_audio_is_streaming(void);

/**
 * 读 mono PCM（已选声道 + 去直流）。
 * @param out 输出缓冲
 * @param samples 期望样点数
 * @param timeout_ms I2S 读超时
 * @param out_got 实际得到样点数（可 NULL）
 */
status_t desktop_pet_audio_stream_read_mono(int16_t *out, size_t samples, uint32_t timeout_ms, size_t *out_got);

/**
 * 流式播放（Z1-4）：与 Rec/stream 互斥。调用方写入 mono PCM。
 */
status_t desktop_pet_audio_playout_start(void);
status_t desktop_pet_audio_playout_stop(void);
bool desktop_pet_audio_is_playouting(void);
status_t desktop_pet_audio_playout_write_mono(const int16_t *pcm, size_t samples, uint32_t timeout_ms);

/**
 * 从绝对路径播放 16 kHz mono 16-bit PCM WAV（照料提示音）。
 * 与 recording / stream / playout / 内存 play 互斥；可打断上一句文件播放。
 * 超过约 8 s 的 data 截断播放。
 */
status_t desktop_pet_audio_play_wav_path(const char *abs_path);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
