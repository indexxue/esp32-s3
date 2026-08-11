/**
 * @file camera_app_input.h
 * @brief BTN1 + 12V 继电器。
 */

#ifndef CAMERA_APP_INPUT_H
#define CAMERA_APP_INPUT_H

#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GPIO1 继电器：上电默认关（低电平）。 */
status_t camera_app_pwr_relay_init(void);

/** 按键扫描任务：短按切换 12V，长按舵机回中，双击 SoftAP 重配网。 */
status_t camera_app_button_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_APP_INPUT_H */
