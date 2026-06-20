#ifndef COMMON_BUTTON_H
#define COMMON_BUTTON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_STATUS_UP = 0,
    BTN_STATUS_DOWN,
} btn_status_e;

typedef enum {
    BTN_ID_UP = 0,
    BTN_ID_DOWN,
    BTN_ID_LEFT,
    BTN_ID_RIGHT,
    BTN_ID_MAX_NUMBER,
} btn_id_e;

typedef enum {
    BTN_EVENT_NONE = 0,
    BTN_EVENT_PRESS_DOWN,
    BTN_EVENT_PRESS_UP,
    BTN_EVENT_SINGLE_CLICK,
    BTN_EVENT_DOUBLE_CLICK,
    BTN_EVENT_REPEAT_CLICK,
    BTN_EVENT_LONG_PRESS,
    BTN_EVENT_LONG_HOLD,
    BTN_EVENT_LONG_HOLD_UP,
} btn_event_e;

typedef enum {
    BTN_PERMISSION_NONE = 0,
    BTN_PERMISSION_RESET = (1 << 0),
    BTN_PERMISSION_PAIR = (1 << 1),
    BTN_PERMISSION_FTM = (1 << 2),
    BTN_PERMISSION_BYPASS = (1 << 3),
    BTN_PERMISSION_UNPAIR = (1 << 4),
    BTN_PERMISSION_ALARM = (1 << 5),
    BTN_PERMISSION_UP = (1 << 6),
    BTN_PERMISSION_DOWN = (1 << 7),
    BTN_PERMISSION_LEFT = (1 << 8),
    BTN_PERMISSION_RIGHT = (1 << 9),
    BTN_PERMISSION_CONFIRM = (1 << 10),
    BTN_PERMISSION_BACK = (1 << 11),
} btn_permission_e;

/** 按键事件回调；在实现里打印 id/name/event 即可观察单击、双击、长按等（须配合周期性 button_schedule）。 */
typedef void (*btn_notify_t)(btn_id_e id, const char *name, btn_permission_e permission, btn_event_e event);

/** 须在 nvs_init() 之后调用：按 NVS device_id 选择按键表并注册 GPIO（见 common/src/button.c）。 */
void button_init(btn_notify_t notify);
void button_deinit(void);
/** 按 FLEX_BTN_SCAN_FREQ_HZ（默认 50Hz）周期调用，例如定时器或任务中每 20ms 调一次。 */
void button_schedule(void);
void button_last_event_get(btn_id_e *id, btn_event_e *event);
void button_last_event_clear(void);

const char *button_id_to_str(btn_id_e id);
const char *button_event_to_str(btn_event_e event);

void button_log_notify(btn_id_e id, const char *name, btn_permission_e permission, btn_event_e event);

/** 返回原始 GPIO 电平 0/1；未找到返回 0xFF。 */
uint8_t button_get_level(btn_id_e id);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_BUTTON_H */
