/**
 * @file desktop_pet_selftest.h
 * @brief 桌宠外设厂测/bring-up：上电烟测 + USB 串口 `ptest` 命令。
 */

#ifndef DESKTOP_PET_SELFTEST_H
#define DESKTOP_PET_SELFTEST_H

#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动 USB 命令行，并（可选）上电跑一轮烟测。
 * 串口：`help` / `ptest` / `ptest lcd|bat|imu|led|motor|sd|i2c|audio|all`
 */
status_t desktop_pet_selftest_start(void);

#ifdef __cplusplus
}
#endif

#endif /* DESKTOP_PET_SELFTEST_H */
