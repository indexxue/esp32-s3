#ifndef PROJECT_MAIN_START_H
#define PROJECT_MAIN_START_H

/**
 * @file start.h
 * @brief 应用生命周期壳层：在 app_main 上下文中顺序执行 init → run（均允许为 NULL）。
 *       `app_entry` 为本工程默认路径：板级与按键/灯效见 start.c，LCD/SD/IMU 等业务任务见 main.c。
 *       约定说明见 doc/application_architecture.md。
 */

#include "type.h"

/**
 * run 为 NULL 时的默认 idle 周期，以及可与 app_run 对齐的主线程延时粒度（毫秒）。
 * 可在包含本头文件之前 #undef / #define 覆盖。
 */
#ifndef APP_LIFECYCLE_IDLE_DELAY_MS
#define APP_LIFECYCLE_IDLE_DELAY_MS (1000U)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef status_t (*app_init_fn_t)(void);
typedef void (*app_run_fn_t)(void);

/** 单次 init + 主循环 run；成员可为 NULL 表示跳过该阶段。 */
typedef struct {
    app_init_fn_t init;
    app_run_fn_t run;
} app_lifecycle_t;

/**
 * @param lifecycle 非 NULL；lifecycle->init / ->run 可为 NULL。
 * @return init 的返回值；init 为 NULL 时视为 STATUS_OK。
 *         lifecycle 为 NULL 时返回 STATUS_INVALID_ARG。
 * @note run 为 NULL 时进入低占空 idle 阻塞，不返回。
 *       run 非 NULL 且正常返回时，本函数返回 STATUS_OK（适用于 run 内另起任务后退出）。
 */
status_t app_start(const app_lifecycle_t *lifecycle);

/** 使用内置默认 `app_lifecycle_t` 启动本工程演示应用（等价于 `app_start` + 固定表）。 */
status_t app_entry(void);

/**
 * 创建并启动业务 FreeRTOS 任务（LCD/SD/IMU 等），由 `start.c` 的默认 `app_run` 调用；实现在 `main.c`。
 * 单独头文件非必需：声明放在此处即可让壳层与 `main.c` 共用一个入口 API 头文件。
 */
status_t application_start_modules_task(void);

#ifdef __cplusplus
}
#endif

#endif
