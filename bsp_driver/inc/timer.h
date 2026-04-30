#ifndef DRIVER_TIMER_H
#define DRIVER_TIMER_H

#include "type.h"

typedef enum {
    TIMER_CLOCK_SRC_DEFAULT_E = 0,
    TIMER_CLOCK_SRC_APB_E = 1,
    TIMER_CLOCK_SRC_XTAL_E = 2,
    TIMER_CLOCK_SRC_RC_FAST_E = 3
} TimerClockSource_t;

typedef enum {
    TIMER_COUNT_UP_E = 0,
    TIMER_COUNT_DOWN_E = 1
} TimerCountDirection_t;

typedef u8_t TimerHandle_t;

#define TIMER_HANDLE_INVALID ((TimerHandle_t)0xFFU)

typedef struct {
    TimerClockSource_t clockSource;
    TimerCountDirection_t countDirection;
    u32_t resolutionHz;
    s32_t intrPriority;
} TimerConfig_t;

typedef struct {
    u64_t alarmCount;
    u64_t reloadCount;
    bool_t autoReload;
} TimerAlarmConfig_t;

typedef void (*TimerCallback_t)(TimerHandle_t handle, void *userData);

bool_t TimerDriverInit(void);
bool_t TimerDriverDeinit(void);

bool_t TimerCreate(const TimerConfig_t *config, TimerHandle_t *outHandle);
bool_t TimerDelete(TimerHandle_t handle);

bool_t TimerSetAlarm(TimerHandle_t handle, const TimerAlarmConfig_t *alarmConfig);
bool_t TimerRegisterCallback(TimerHandle_t handle, TimerCallback_t callback, void *userData);

bool_t TimerStart(TimerHandle_t handle);
bool_t TimerStop(TimerHandle_t handle);
bool_t TimerSetCount(TimerHandle_t handle, u64_t countValue);
bool_t TimerGetCount(TimerHandle_t handle, u64_t *countValue);

s32_t TimerGetLastError(void);

#endif
