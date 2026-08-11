/**
 * @file camera_app_config.h
 * @brief camera 编译模式开关（由 CMake 注入，本地可 #ifndef 兜底）。
 *
 * DETECT=1 识别；COLLECT=1 采数；CALIB=1 舵机校准。
 * 默认均为 0：预览 + Web + 舵机 + SPI + 按键继电器（不开 ESPDet）。
 */

#ifndef CAMERA_APP_CONFIG_H
#define CAMERA_APP_CONFIG_H

#ifndef CAMERA_APP_DETECT_MODE
#define CAMERA_APP_DETECT_MODE 0
#endif

#ifndef CAMERA_APP_COLLECT_MODE
#define CAMERA_APP_COLLECT_MODE 0
#endif

#ifndef CAMERA_APP_CALIB_MODE
#define CAMERA_APP_CALIB_MODE 0
#endif

#if (CAMERA_APP_CALIB_MODE) && (CAMERA_APP_COLLECT_MODE)
#error "CAMERA_APP_CALIB_MODE and CAMERA_APP_COLLECT_MODE are mutually exclusive"
#endif

#if (CAMERA_APP_DETECT_MODE) && ((CAMERA_APP_COLLECT_MODE) || (CAMERA_APP_CALIB_MODE))
#error "CAMERA_APP_DETECT_MODE cannot combine with COLLECT or CALIB"
#endif

#endif /* CAMERA_APP_CONFIG_H */
