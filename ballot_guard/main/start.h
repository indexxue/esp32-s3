#ifndef BALLOT_GUARD_START_H
#define BALLOT_GUARD_START_H

/**
 * @file start.h
 * @brief ballot_guard 生命周期壳层：init → run。
 */

#include "type.h"

#ifndef APP_LIFECYCLE_IDLE_DELAY_MS
#define APP_LIFECYCLE_IDLE_DELAY_MS (1000U)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef status_t (*app_init_fn_t)(void);
typedef void (*app_run_fn_t)(void);

typedef struct {
    app_init_fn_t init;
    app_run_fn_t  run;
} app_lifecycle_t;

status_t app_start(const app_lifecycle_t *lifecycle);
status_t app_entry(void);

#ifdef __cplusplus
}
#endif

#endif
