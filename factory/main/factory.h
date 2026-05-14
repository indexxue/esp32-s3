#ifndef FACTORY_MAIN_FACTORY_H
#define FACTORY_MAIN_FACTORY_H

/**
 * @file factory.h
 * @brief 厂测工程生命周期壳层（与 `project/main/start.h` 同形，入口名为 `factory_entry`）。
 *       业务逻辑请写在 `main.c` 或本目录其它 .c 文件，勿与量产 `app_entry` 混用。
 */

#include "type.h"

#ifndef APP_LIFECYCLE_IDLE_DELAY_MS
#define APP_LIFECYCLE_IDLE_DELAY_MS (1000U)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef status_t (*factory_init_fn_t)(void);
typedef void (*factory_run_fn_t)(void);

typedef struct {
    factory_init_fn_t init;
    factory_run_fn_t run;
} factory_lifecycle_t;

status_t factory_lifecycle_start(const factory_lifecycle_t *lifecycle);

/** 默认厂测壳层：板级 / 按键 / 灯效 / USB 厂测命令；不含量产 `main.c` 里的 LCD/SD/IMU 业务任务。 */
status_t factory_entry(void);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_MAIN_FACTORY_H */
