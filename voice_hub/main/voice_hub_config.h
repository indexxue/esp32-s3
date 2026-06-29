/**
 * @file voice_hub_config.h
 * @brief voice_hub 模块开关：填好 board.h（BOARD_PROFILE_VOICE_HUB 段）引脚后按 M1→M3 逐项置 1 bring-up。
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

/** M2：OV2640 低帧率预览 */
#ifndef VOICE_HUB_ENABLE_CAMERA
#define VOICE_HUB_ENABLE_CAMERA 0
#endif

/** M3：按需拍照 → SD（启用后由 voice_hub_storage_init 挂载） */
#ifndef VOICE_HUB_ENABLE_SDCARD
#define VOICE_HUB_ENABLE_SDCARD 0
#endif

/** 预览默认 QVGA；可在 camera 模块内再调。 */
#define VOICE_HUB_CAMERA_PREVIEW_WIDTH (320U)
#define VOICE_HUB_CAMERA_PREVIEW_HEIGHT (240U)

#endif /* VOICE_HUB_CONFIG_H */
