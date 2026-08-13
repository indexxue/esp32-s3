/**
 * @file pet_opus.c
 * @brief Opus 编解码（78/esp-opus）：上行 VOIP 编码 + 下行解码。
 */

#include "pet_opus.h"

#include "audio.h"
#include "log.h"

#include "opus.h"

#include <stdlib.h>
#include <string.h>

static OpusEncoder *s_enc;
static OpusDecoder *s_dec;
static int s_dec_rate_hz;

status_t desktop_pet_opus_enc_init(void)
{
    int err = OPUS_OK;

    if (s_enc != NULL) {
        return STATUS_OK;
    }

    s_enc = opus_encoder_create((opus_int32)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
    if (s_enc == NULL || err != OPUS_OK) {
        LOG_ERROR("opus: enc create failed err=%d", err);
        s_enc = NULL;
        return STATUS_FAIL;
    }

    (void)opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(24000));
    /* complexity 越高栈越深；5 在 pet_up 上曾栈溢出，降到 3 换稳定性。 */
    (void)opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(3));
    (void)opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    LOG_INFO("opus: enc ready 16kHz mono 60ms");
    return STATUS_OK;
}

void desktop_pet_opus_enc_deinit(void)
{
    if (s_enc != NULL) {
        opus_encoder_destroy(s_enc);
        s_enc = NULL;
    }
}

bool desktop_pet_opus_enc_is_ready(void)
{
    return s_enc != NULL;
}

status_t desktop_pet_opus_encode_frame(const int16_t *pcm, uint8_t *out, size_t out_cap, size_t *out_len)
{
    int n;

    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (s_enc == NULL || pcm == NULL || out == NULL || out_cap < 16U) {
        return STATUS_INVALID_ARG;
    }

    n = opus_encode(s_enc, pcm, (int)DESKTOP_PET_OPUS_FRAME_SAMPLES, out, (opus_int32)out_cap);
    if (n < 0) {
        LOG_WARN("opus: encode err %d", n);
        return STATUS_FAIL;
    }
    if (out_len != NULL) {
        *out_len = (size_t)n;
    }
    return STATUS_OK;
}

status_t desktop_pet_opus_dec_init(int sample_rate_hz)
{
    int err = OPUS_OK;
    int rate = sample_rate_hz;

    if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) {
        rate = (int)DESKTOP_PET_AUDIO_SAMPLE_RATE_HZ;
    }

    if (s_dec != NULL && s_dec_rate_hz == rate) {
        return STATUS_OK;
    }
    desktop_pet_opus_dec_deinit();

    s_dec = opus_decoder_create((opus_int32)rate, 1, &err);
    if (s_dec == NULL || err != OPUS_OK) {
        LOG_ERROR("opus: dec create failed err=%d rate=%d", err, rate);
        s_dec = NULL;
        s_dec_rate_hz = 0;
        return STATUS_FAIL;
    }
    s_dec_rate_hz = rate;
    LOG_INFO("opus: dec ready %dHz mono", rate);
    return STATUS_OK;
}

void desktop_pet_opus_dec_deinit(void)
{
    if (s_dec != NULL) {
        opus_decoder_destroy(s_dec);
        s_dec = NULL;
    }
    s_dec_rate_hz = 0;
}

bool desktop_pet_opus_dec_is_ready(void)
{
    return s_dec != NULL;
}

int desktop_pet_opus_dec_sample_rate(void)
{
    return s_dec_rate_hz;
}

status_t desktop_pet_opus_decode_frame(const uint8_t *in, size_t in_len, int16_t *pcm, size_t pcm_cap,
                                       size_t *out_samples)
{
    int n;
    int frame_samples;

    if (out_samples != NULL) {
        *out_samples = 0U;
    }
    if (s_dec == NULL || in == NULL || in_len == 0U || pcm == NULL || pcm_cap < 80U) {
        return STATUS_INVALID_ARG;
    }

    frame_samples = (s_dec_rate_hz > 0) ? (s_dec_rate_hz * 60 / 1000) : (int)DESKTOP_PET_OPUS_FRAME_SAMPLES;
    if (frame_samples > (int)pcm_cap) {
        frame_samples = (int)pcm_cap;
    }

    n = opus_decode(s_dec, in, (opus_int32)in_len, pcm, frame_samples, 0);
    if (n < 0) {
        LOG_WARN("opus: decode err %d", n);
        return STATUS_FAIL;
    }
    if (out_samples != NULL) {
        *out_samples = (size_t)n;
    }
    return STATUS_OK;
}
