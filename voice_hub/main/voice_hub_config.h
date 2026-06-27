/**
 * @file voice_hub_config.h
 * @brief voice_hub 模块开关：填好 board.h（BOARD_PROFILE_VOICE_HUB 段）引脚后按 M1→M5 逐项置 1 bring-up。
 */

#ifndef VOICE_HUB_CONFIG_H
#define VOICE_HUB_CONFIG_H

/** M1：ST7789 + 单键 + Wi-Fi/Web/OTA */
#ifndef VOICE_HUB_ENABLE_LCD
#define VOICE_HUB_ENABLE_LCD 1
#endif

#ifndef VOICE_HUB_ENABLE_WIFI_WEB
#define VOICE_HUB_ENABLE_WIFI_WEB 1
#endif

/** M2：ES8311 播放/录音 */
#ifndef VOICE_HUB_ENABLE_AUDIO
#define VOICE_HUB_ENABLE_AUDIO 0
#endif

/** M3：OV2640 低帧率预览 */
#ifndef VOICE_HUB_ENABLE_CAMERA
#define VOICE_HUB_ENABLE_CAMERA 0
#endif

/** M4：按需拍照 → SD */
#ifndef VOICE_HUB_ENABLE_SDCARD
#define VOICE_HUB_ENABLE_SDCARD 0
#endif

/** M5：全双工对讲 Web 音频流 */
#ifndef VOICE_HUB_ENABLE_INTERCOM
#define VOICE_HUB_ENABLE_INTERCOM 0
#endif

/** 预览默认 QVGA；可在 camera 模块内再调。 */
#define VOICE_HUB_CAMERA_PREVIEW_WIDTH (320U)
#define VOICE_HUB_CAMERA_PREVIEW_HEIGHT (240U)

#endif /* VOICE_HUB_CONFIG_H */
