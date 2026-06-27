/**
 * @file voice_hub_audio.h
 * @brief ES8311 播放/录音/全双工对讲骨架（M2/M5）。
 */

#pragma once

#include "type.h"

status_t voice_hub_audio_init(void);
bool_t voice_hub_audio_is_ready(void);
/** NS4150B PA_EN：允许/关闭功放输出（播放前须使能）。 */
status_t voice_hub_audio_pa_set(bool_t enable);
status_t voice_hub_audio_play_prompt(const char *tag);
status_t voice_hub_audio_intercom_start(void);
status_t voice_hub_audio_intercom_stop(void);
