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

/** Web JPEG 轮询最小间隔（ms）；须大于单帧编码+传输耗时，避免 httpd 连接堆叠。 */
#define VOICE_HUB_CAMERA_WEB_FRAME_MS (200U)

/** Web JPEG 质量 1–100。 */
#define VOICE_HUB_CAMERA_WEB_JPEG_QUALITY (65U)

/**
 * LCD blit SPI 条带字节上限（须为整行 RGB565 的倍数）。
 * 全板 BOARD_ST7789_SPI_MAX_TX 常为 32KiB，与 WiFi 共用 MALLOC_CAP_DMA 时易耗尽导致重启。
 */
#ifndef VOICE_HUB_CAMERA_BLIT_STRIP_BYTES_MAX
#define VOICE_HUB_CAMERA_BLIT_STRIP_BYTES_MAX (8192U)
#endif

/** 预览/Web 共用 RGB565 快照字节数（240×240×2）。 */
#define VOICE_HUB_CAMERA_SNAPSHOT_BYTES \
    ((uint32_t)VOICE_HUB_CAMERA_PREVIEW_WIDTH * (uint32_t)VOICE_HUB_CAMERA_PREVIEW_HEIGHT * 2U)

/**
 * 摄像头画面方向（LCD 与 Web 共用同一套归一化处理，保证一致）。
 * 若镜像/上下颠倒，只需改此处宏后重新编译。
 */
#ifndef VOICE_HUB_CAMERA_FLIP_VERTICAL
#define VOICE_HUB_CAMERA_FLIP_VERTICAL 1
#endif
#ifndef VOICE_HUB_CAMERA_FLIP_HORIZONTAL
#define VOICE_HUB_CAMERA_FLIP_HORIZONTAL 0
#endif

/** ST7789 使用 MADCTL_BGR 时置 1，将 RGB565 转为面板 BGR565（仅 LCD 路径）。 */
#ifndef VOICE_HUB_CAMERA_PANEL_SWAP_RB
#define VOICE_HUB_CAMERA_PANEL_SWAP_RB 1
#endif

/** LCD 右上角 IP 文字：距顶/距右像素（显示坐标系，已含翻转后方向）。 */
#define VOICE_HUB_UI_IP_MARGIN_X (2U)
#define VOICE_HUB_UI_IP_MARGIN_Y (1U)
/** IP 字体高度（点阵 sizey）；预览 blit 跳过该条带，避免每帧覆盖后重绘闪烁。 */
#define VOICE_HUB_UI_IP_FONT_SIZE (12U)
#define VOICE_HUB_UI_IP_BAND_H (VOICE_HUB_UI_IP_MARGIN_Y + VOICE_HUB_UI_IP_FONT_SIZE)

/** 后台查询 IP 间隔（ms）；GOT_IP 事件会立即刷新。 */
#define VOICE_HUB_UI_IP_REFRESH_MS (3000U)

#endif /* VOICE_HUB_CONFIG_H */
