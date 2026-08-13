/**
 * @file desktop_pet_audio.h
 * @brief ES8311 + I2S record/play; PCM in PSRAM, stop writes WAV under /sdcard/record/.
 */

#ifndef DESKTOP_PET_AUDIO_H
#define DESKTOP_PET_AUDIO_H

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

#ifdef __cplusplus
}
#endif

#endif /* DESKTOP_PET_AUDIO_H */
