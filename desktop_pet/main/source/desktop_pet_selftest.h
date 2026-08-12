/**
 * @file desktop_pet_selftest.h
 * @brief 桌宠外设厂测/bring-up（参考程序，默认不参与编译）。
 *
 * 恢复接入：
 * 1. main/CMakeLists.txt 的 SRCS 加回 source/desktop_pet_selftest.c
 * 2. start.c 在 UI 前调用 desktop_pet_selftest_start()
 * 3. board.h 中 DESKTOP_PET_ENABLE_SELFTEST 置 1
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
