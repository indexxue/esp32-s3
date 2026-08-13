/**
 * @file desktop_pet_opus.h
 * @brief PCM(16 kHz mono) ↔ Opus 帧封装（上行编码 + 下行解码）。
 */

#ifndef DESKTOP_PET_OPUS_H
#define DESKTOP_PET_OPUS_H

#include "type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 与 hello audio_params.frame_duration=60 对齐：16kHz * 60ms = 960。 */
#define DESKTOP_PET_OPUS_FRAME_SAMPLES (960U)
/** 服务端常见 24kHz * 60ms。 */
#define DESKTOP_PET_OPUS_FRAME_SAMPLES_24K (1440U)
#define DESKTOP_PET_OPUS_MAX_PACKET (512U)
#define DESKTOP_PET_OPUS_PCM_MAX_SAMPLES (2880U)

status_t desktop_pet_opus_enc_init(void);
void desktop_pet_opus_enc_deinit(void);
bool desktop_pet_opus_enc_is_ready(void);

/**
 * 编码一帧 mono PCM。
 * @param pcm 长度须为 DESKTOP_PET_OPUS_FRAME_SAMPLES
 * @param out Opus 包缓冲
 * @param out_cap out 容量
 * @param out_len 实际字节数
 */
status_t desktop_pet_opus_encode_frame(const int16_t *pcm, uint8_t *out, size_t out_cap, size_t *out_len);

status_t desktop_pet_opus_dec_init(int sample_rate_hz);
void desktop_pet_opus_dec_deinit(void);
bool desktop_pet_opus_dec_is_ready(void);
int desktop_pet_opus_dec_sample_rate(void);

/**
 * 解码一包 Opus → mono PCM。
 * @param out_samples 输出样点数（随编码器帧长变化）
 */
status_t desktop_pet_opus_decode_frame(const uint8_t *in, size_t in_len, int16_t *pcm, size_t pcm_cap,
                                       size_t *out_samples);

#ifdef __cplusplus
}
#endif

#endif /* DESKTOP_PET_OPUS_H */
