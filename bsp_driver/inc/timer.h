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

/** 与 FreeRTOS 的 TimerHandle_t（软件定时器）区分 */
typedef u8_t GptimerHandle_t;

#define GPTIMER_HANDLE_INVALID ((GptimerHandle_t)0xFFU)

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

typedef void (*TimerCallback_t)(GptimerHandle_t handle, void *userData);

bool_t TimerDriverInit(void);
bool_t TimerDriverDeinit(void);

bool_t TimerCreate(const TimerConfig_t *config, GptimerHandle_t *outHandle);
bool_t TimerDelete(GptimerHandle_t handle);

bool_t TimerSetAlarm(GptimerHandle_t handle, const TimerAlarmConfig_t *alarmConfig);
bool_t TimerRegisterCallback(GptimerHandle_t handle, TimerCallback_t callback, void *userData);

bool_t TimerStart(GptimerHandle_t handle);
bool_t TimerStop(GptimerHandle_t handle);
bool_t TimerSetCount(GptimerHandle_t handle, u64_t countValue);
bool_t TimerGetCount(GptimerHandle_t handle, u64_t *countValue);

s32_t TimerGetLastError(void);

#endif
