#include "timer.h"

#include <stddef.h>

#include "driver/gptimer.h"
#include "esp_attr.h"

#define TIMER_INSTANCE_MAX (4U)

typedef struct {
    bool_t inUse;
    gptimer_handle_t gptimerHandle;
    TimerCallback_t callback;
    void *callbackUserData;
} TimerSlot_t;

static bool_t s_timerDriverInited = FALSE;
static TimerSlot_t s_timerSlots[TIMER_INSTANCE_MAX] = {0};
static esp_err_t s_timerLastErr = ESP_OK;

static bool_t timerSetLastErr(esp_err_t err)
{
    s_timerLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

static bool_t timerIsHandleValid(TimerHandle_t handle)
{
    return (handle < TIMER_INSTANCE_MAX) ? TRUE : FALSE;
}

static bool_t timerGetSlotByHandle(TimerHandle_t handle, TimerSlot_t **slot)
{
    if (slot == NULL) {
        return timerSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_timerDriverInited == FALSE) {
        return timerSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (timerIsHandleValid(handle) == FALSE) {
        return timerSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_timerSlots[handle].inUse == FALSE) {
        return timerSetLastErr(ESP_ERR_NOT_FOUND);
    }

    *slot = &s_timerSlots[handle];
    return timerSetLastErr(ESP_OK);
}

static s32_t timerFindFreeSlotIndex(void)
{
    u8_t i = 0U;

    for (i = 0U; i < TIMER_INSTANCE_MAX; i++) {
        if (s_timerSlots[i].inUse == FALSE) {
            return (s32_t)i;
        }
    }

    return -1;
}

static bool_t timerConvertClockSource(TimerClockSource_t clockSource, gptimer_clock_source_t *outClockSource)
{
    if (outClockSource == NULL) {
        return timerSetLastErr(ESP_ERR_INVALID_ARG);
    }

    switch (clockSource) {
        case TIMER_CLOCK_SRC_DEFAULT_E: *outClockSource = GPTIMER_CLK_SRC_DEFAULT; break;
#if defined(SOC_TIMER_GROUP_SUPPORT_APB) && SOC_TIMER_GROUP_SUPPORT_APB
        case TIMER_CLOCK_SRC_APB_E: *outClockSource = GPTIMER_CLK_SRC_APB; break;
#endif
#if defined(SOC_TIMER_GROUP_SUPPORT_XTAL) && SOC_TIMER_GROUP_SUPPORT_XTAL
        case TIMER_CLOCK_SRC_XTAL_E: *outClockSource = GPTIMER_CLK_SRC_XTAL; break;
#endif
#if defined(SOC_TIMER_GROUP_SUPPORT_RC_FAST) && SOC_TIMER_GROUP_SUPPORT_RC_FAST
        case TIMER_CLOCK_SRC_RC_FAST_E: *outClockSource = GPTIMER_CLK_SRC_RC_FAST; break;
#endif
        default: return timerSetLastErr(ESP_ERR_NOT_SUPPORTED);
    }

    return timerSetLastErr(ESP_OK);
}

static bool_t timerConvertCountDirection(TimerCountDirection_t direction, gptimer_count_direction_t *outDirection)
{
    if (outDirection == NULL) {
        return timerSetLastErr(ESP_ERR_INVALID_ARG);
    }

    switch (direction) {
        case TIMER_COUNT_UP_E: *outDirection = GPTIMER_COUNT_UP; break;
        case TIMER_COUNT_DOWN_E: *outDirection = GPTIMER_COUNT_DOWN; break;
        default: return timerSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return timerSetLastErr(ESP_OK);
}

static bool IRAM_ATTR timerOnAlarmCallback(gptimer_handle_t timer,
                                           const gptimer_alarm_event_data_t *eventData,
                                           void *userCtx)
{
    TimerSlot_t *slot = (TimerSlot_t *)userCtx;
    (void)timer;
    (void)eventData;

    if ((slot != NULL) && (slot->callback != NULL)) {
        TimerHandle_t handle = (TimerHandle_t)(slot - &s_timerSlots[0]);
        slot->callback(handle, slot->callbackUserData);
    }

    return false;
}

bool_t TimerDriverInit(void)
{
    u8_t i = 0U;

    if (s_timerDriverInited == TRUE) {
        return timerSetLastErr(ESP_OK);
    }

    for (i = 0U; i < TIMER_INSTANCE_MAX; i++) {
        s_timerSlots[i].inUse = FALSE;
        s_timerSlots[i].gptimerHandle = NULL;
        s_timerSlots[i].callback = NULL;
        s_timerSlots[i].callbackUserData = NULL;
    }

    s_timerDriverInited = TRUE;
    return timerSetLastErr(ESP_OK);
}

bool_t TimerDriverDeinit(void)
{
    u8_t i = 0U;
    esp_err_t ret = ESP_OK;

    if (s_timerDriverInited == FALSE) {
        return timerSetLastErr(ESP_OK);
    }

    for (i = 0U; i < TIMER_INSTANCE_MAX; i++) {
        if (s_timerSlots[i].inUse == TRUE) {
            ret = gptimer_stop(s_timerSlots[i].gptimerHandle);
            if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
                return timerSetLastErr(ret);
            }

            ret = gptimer_disable(s_timerSlots[i].gptimerHandle);
            if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
                return timerSetLastErr(ret);
            }

            ret = gptimer_del_timer(s_timerSlots[i].gptimerHandle);
            if (ret != ESP_OK) {
                return timerSetLastErr(ret);
            }

            s_timerSlots[i].inUse = FALSE;
            s_timerSlots[i].gptimerHandle = NULL;
            s_timerSlots[i].callback = NULL;
            s_timerSlots[i].callbackUserData = NULL;
        }
    }

    s_timerDriverInited = FALSE;
    return timerSetLastErr(ESP_OK);
}

bool_t TimerCreate(const TimerConfig_t *config, TimerHandle_t *outHandle)
{
    gptimer_config_t gptimerConfig = {0};
    gptimer_clock_source_t clockSource = GPTIMER_CLK_SRC_DEFAULT;
    gptimer_count_direction_t countDirection = GPTIMER_COUNT_UP;
    gptimer_handle_t gptimerHandle = NULL;
    s32_t slotIndex = -1;
    esp_err_t ret = ESP_OK;

    if (s_timerDriverInited == FALSE) {
        return timerSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if ((config == NULL) || (outHandle == NULL)) {
        return timerSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (config->resolutionHz == 0U) {
        return timerSetLastErr(ESP_ERR_INVALID_ARG);
    }

    slotIndex = timerFindFreeSlotIndex();
    if (slotIndex < 0) {
        return timerSetLastErr(ESP_ERR_NO_MEM);
    }

    if (timerConvertClockSource(config->clockSource, &clockSource) == FALSE) {
        return FALSE;
    }
    if (timerConvertCountDirection(config->countDirection, &countDirection) == FALSE) {
        return FALSE;
    }

    gptimerConfig.clk_src = clockSource;
    gptimerConfig.direction = countDirection;
    gptimerConfig.resolution_hz = config->resolutionHz;
    gptimerConfig.intr_priority = config->intrPriority;

    ret = gptimer_new_timer(&gptimerConfig, &gptimerHandle);
    if (ret != ESP_OK) {
        return timerSetLastErr(ret);
    }

    s_timerSlots[slotIndex].inUse = TRUE;
    s_timerSlots[slotIndex].gptimerHandle = gptimerHandle;
    s_timerSlots[slotIndex].callback = NULL;
    s_timerSlots[slotIndex].callbackUserData = NULL;
    *outHandle = (TimerHandle_t)slotIndex;
    return timerSetLastErr(ESP_OK);
}

bool_t TimerDelete(TimerHandle_t handle)
{
    TimerSlot_t *slot = NULL;
    esp_err_t ret = ESP_OK;

    if (timerGetSlotByHandle(handle, &slot) == FALSE) {
        return FALSE;
    }

    ret = gptimer_stop(slot->gptimerHandle);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return timerSetLastErr(ret);
    }

    ret = gptimer_disable(slot->gptimerHandle);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return timerSetLastErr(ret);
    }

    ret = gptimer_del_timer(slot->gptimerHandle);
    if (ret != ESP_OK) {
        return timerSetLastErr(ret);
    }

    slot->inUse = FALSE;
    slot->gptimerHandle = NULL;
    slot->callback = NULL;
    slot->callbackUserData = NULL;
    return timerSetLastErr(ESP_OK);
}

bool_t TimerSetAlarm(TimerHandle_t handle, const TimerAlarmConfig_t *alarmConfig)
{
    TimerSlot_t *slot = NULL;
    gptimer_alarm_config_t gptimerAlarmConfig = {0};
    esp_err_t ret = ESP_OK;

    if (alarmConfig == NULL) {
        return timerSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (timerGetSlotByHandle(handle, &slot) == FALSE) {
        return FALSE;
    }

    gptimerAlarmConfig.alarm_count = alarmConfig->alarmCount;
    gptimerAlarmConfig.reload_count = alarmConfig->reloadCount;
    gptimerAlarmConfig.flags.auto_reload_on_alarm = (alarmConfig->autoReload == TRUE) ? 1U : 0U;

    ret = gptimer_set_alarm_action(slot->gptimerHandle, &gptimerAlarmConfig);
    return timerSetLastErr(ret);
}

bool_t TimerRegisterCallback(TimerHandle_t handle, TimerCallback_t callback, void *userData)
{
    TimerSlot_t *slot = NULL;
    gptimer_event_callbacks_t callbacks = {0};
    esp_err_t ret = ESP_OK;

    if (timerGetSlotByHandle(handle, &slot) == FALSE) {
        return FALSE;
    }

    callbacks.on_alarm = (callback != NULL) ? timerOnAlarmCallback : NULL;
    ret = gptimer_register_event_callbacks(slot->gptimerHandle, &callbacks, (void *)slot);
    if (ret != ESP_OK) {
        return timerSetLastErr(ret);
    }

    slot->callback = callback;
    slot->callbackUserData = userData;
    return timerSetLastErr(ESP_OK);
}

bool_t TimerStart(TimerHandle_t handle)
{
    TimerSlot_t *slot = NULL;
    esp_err_t ret = ESP_OK;

    if (timerGetSlotByHandle(handle, &slot) == FALSE) {
        return FALSE;
    }

    ret = gptimer_enable(slot->gptimerHandle);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return timerSetLastErr(ret);
    }

    ret = gptimer_start(slot->gptimerHandle);
    return timerSetLastErr(ret);
}

bool_t TimerStop(TimerHandle_t handle)
{
    TimerSlot_t *slot = NULL;
    esp_err_t ret = ESP_OK;

    if (timerGetSlotByHandle(handle, &slot) == FALSE) {
        return FALSE;
    }

    ret = gptimer_stop(slot->gptimerHandle);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return timerSetLastErr(ret);
    }

    ret = gptimer_disable(slot->gptimerHandle);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return timerSetLastErr(ret);
    }

    return timerSetLastErr(ESP_OK);
}

bool_t TimerSetCount(TimerHandle_t handle, u64_t countValue)
{
    TimerSlot_t *slot = NULL;
    esp_err_t ret = ESP_OK;

    if (timerGetSlotByHandle(handle, &slot) == FALSE) {
        return FALSE;
    }

    ret = gptimer_set_raw_count(slot->gptimerHandle, countValue);
    return timerSetLastErr(ret);
}

bool_t TimerGetCount(TimerHandle_t handle, u64_t *countValue)
{
    TimerSlot_t *slot = NULL;
    esp_err_t ret = ESP_OK;

    if (countValue == NULL) {
        return timerSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (timerGetSlotByHandle(handle, &slot) == FALSE) {
        return FALSE;
    }

    ret = gptimer_get_raw_count(slot->gptimerHandle, countValue);
    return timerSetLastErr(ret);
}

s32_t TimerGetLastError(void)
{
    return (s32_t)s_timerLastErr;
}
