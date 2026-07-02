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
#define VOICE_HUB_ENABLE_CAMERA 1
#endif

/** M3：按需拍照 → SD（启用后由 voice_hub_storage_init 挂载） */
#ifndef VOICE_HUB_ENABLE_SDCARD
#define VOICE_HUB_ENABLE_SDCARD 0
#endif

/** M3 bring-up：上电挂载成功后跑 FAT 读写烟测 + 吞吐日志。 */
#ifndef VOICE_HUB_RUN_SDCARD_RW_TEST_ON_BOOT
#define VOICE_HUB_RUN_SDCARD_RW_TEST_ON_BOOT 0
#endif

/** 预览 240×240 RGB565，LCD 240×135 居中裁剪显示。 */
#define VOICE_HUB_CAMERA_PREVIEW_WIDTH (240U)
#define VOICE_HUB_CAMERA_PREVIEW_HEIGHT (240U)
#define VOICE_HUB_CAMERA_SENSOR_FORMAT "DVP_8bit_20Minput_RGB565_BE_240x240_25fps"

#endif /* VOICE_HUB_CONFIG_H */
