/**
 * @file servo_calib_cmd.h
 * @brief 校准固件 USB 串口舵机命令（不依赖网页）。
 */

#ifndef CAMERA_SERVO_CALIB_CMD_H
#define CAMERA_SERVO_CALIB_CMD_H

/** 注册 `servo` 命令；供 `cmd_set_project_register_fn` 使用。 */
void servo_calib_cmd_register(void);

#endif /* CAMERA_SERVO_CALIB_CMD_H */
