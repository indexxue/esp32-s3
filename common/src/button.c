#include "button.h"

#include <string.h>

#include "device_profile.h"
#include "flexible_button.h"
#include "gpio.h"
#include "nvs.h"

#if defined(BUTTON_USE_LOG) && (BUTTON_USE_LOG)
#include "esp_log.h"
#endif

/** 各项目按键表最大路数；新增项目时按需增大。 */
#define BUTTON_MAX_NUM 6U

typedef struct {
    btn_id_e id;
    const char *name;
    int32_t gpio;
    uint8_t active_level;
    uint16_t permission;
    flex_button_t flex;
} button_list_t;

typedef struct {
    uint8_t num;
    button_list_t *list;
    btn_notify_t notify;
} button_item_t;

static button_list_t s_button_list[BUTTON_MAX_NUM];
static button_item_t self = {0};

static btn_id_e last_button_id = BTN_ID_MAX_NUMBER;
static btn_event_e last_button_event = BTN_EVENT_NONE;

static uint8_t button_read_gpio(int32_t gpio)
{
    u32_t level = 0U;
    if (GpioReadPin(gpio, &level) == FALSE) {
        return 0U;
    }
    return (uint8_t)level;
}

static uint8_t button_flex_read(void *flex)
{
    flex_button_t *btn = (flex_button_t *)flex;
    button_list_t *list = (button_list_t *)btn->user_data;
    return button_read_gpio(list->gpio);
}

static bool button_is_permission(button_list_t *list, btn_permission_e permission)
{
    return (list->permission & (uint16_t)permission) != 0u;
}

static void button_flex_press_long_process(flex_button_t *flex, button_list_t *list)
{
    (void)flex;
    if (!button_is_permission(list, BTN_PERMISSION_RESET)) {
        return;
    }
}

static void button_flex_event_callback(void *arg)
{
    flex_button_t *flex = (flex_button_t *)arg;
    button_list_t *list = (button_list_t *)flex->user_data;
    flex_button_event_t fevt = flex_button_event_read(flex);
    btn_event_e bevt = BTN_EVENT_NONE;

    switch (fevt) {
        case FLEX_BTN_PRESS_DOWN:
            bevt = BTN_EVENT_PRESS_DOWN;
            break;
        case FLEX_BTN_PRESS_CLICK:
            bevt = BTN_EVENT_SINGLE_CLICK;
            break;
        case FLEX_BTN_PRESS_DOUBLE_CLICK:
            bevt = BTN_EVENT_DOUBLE_CLICK;
            break;
        case FLEX_BTN_PRESS_REPEAT_CLICK:
            bevt = BTN_EVENT_REPEAT_CLICK;
            break;
        case FLEX_BTN_PRESS_LONG_START:
            bevt = BTN_EVENT_LONG_PRESS;
            break;
        case FLEX_BTN_PRESS_LONG_HOLD:
            bevt = BTN_EVENT_LONG_HOLD;
            break;
        case FLEX_BTN_PRESS_LONG_HOLD_UP:
            bevt = BTN_EVENT_LONG_HOLD_UP;
            break;
        default:
            bevt = BTN_EVENT_NONE;
            break;
    }

    if (bevt != BTN_EVENT_NONE) {
        last_button_id = list->id;
        last_button_event = bevt;
        if (self.notify != NULL) {
            self.notify(list->id, list->name, (btn_permission_e)list->permission, bevt);
        }
        button_flex_press_long_process(flex, list);
    }
}

static void button_flex_init(button_list_t *list)
{
    memset(&list->flex, 0, sizeof(flex_button_t));
    list->flex.usr_button_read = button_flex_read;
    list->flex.cb = button_flex_event_callback;
    list->flex.pressed_logic_level = (list->active_level ? 1u : 0u);
    list->flex.debounce_tick = FLEX_MS_TO_SCAN_CNT(80);
    list->flex.max_multiple_clicks_interval = FLEX_MS_TO_SCAN_CNT(600);
    list->flex.short_press_start_tick = FLEX_MS_TO_SCAN_CNT(2000);
    list->flex.long_press_start_tick = FLEX_MS_TO_SCAN_CNT(10000);
    list->flex.long_hold_start_tick = FLEX_MS_TO_SCAN_CNT(11000);
    list->flex.user_data = list;

    (void)flex_button_register(&list->flex);
}

static void button_gpio_init(void)
{
    for (uint8_t i = 0; i < self.num; i++) {
        button_list_t *p = &s_button_list[i];
        const GpioPinConfig_t cfg = {
            .pin = p->gpio,
            .mode = GPIO_MODE_INPUT_E,
            .pullUpEn = GPIO_PULL_ENABLE_E,
            .pullDownEn = GPIO_PULL_DISABLE_E,
            .intrType = GPIO_INTR_DISABLE_E,
        };
        (void)GpioConfigurePin(&cfg);
        button_flex_init(p);
    }
}

static void button_config(void)
{
    const device_product_profile_t *product = device_profile_product();
    uint8_t n = device_profile_button_count();

    if (n > BUTTON_MAX_NUM) {
        n = BUTTON_MAX_NUM;
    }

    for (uint8_t i = 0; i < n; i++) {
        const device_button_spec_t *spec = device_profile_button_spec(i);
        button_list_t *entry = &s_button_list[i];

        if (spec == NULL) {
            continue;
        }
        entry->id = spec->id;
        entry->name = spec->name;
        entry->gpio = spec->gpio;
        entry->active_level = spec->active_level;
        entry->permission = spec->permission;
    }

    self.num = n;
    self.list = s_button_list;

#if defined(BUTTON_USE_LOG) && (BUTTON_USE_LOG)
    {
        ESP_LOGI("button", "product=%s (0x%08lX), %u keys", product->name, (unsigned long)product->product_id,
                 (unsigned)n);
    }
#endif
}

void button_init(btn_notify_t notify)
{
    memset(&self, 0, sizeof(button_item_t));
    button_config();
    self.notify = notify;

    (void)GpioDriverInit();
    button_gpio_init();

#if defined(BUTTON_USE_LOG) && (BUTTON_USE_LOG)
    ESP_LOGI("button", "initialized, %u keys", (unsigned)self.num);
#endif
}

void button_schedule(void)
{
    if (self.list == NULL || self.num == 0) {
        return;
    }
    (void)flex_button_scan();
}

void button_deinit(void)
{
    self.list = NULL;
    self.num = 0;
    memset(&self, 0, sizeof(button_item_t));
#if defined(BUTTON_USE_LOG) && (BUTTON_USE_LOG)
    ESP_LOGI("button", "deinitialized");
#endif
}

void button_last_event_get(btn_id_e *id, btn_event_e *event)
{
    if (id != NULL) {
        *id = last_button_id;
    }
    if (event != NULL) {
        *event = last_button_event;
    }
}

void button_last_event_clear(void)
{
    last_button_id = BTN_ID_MAX_NUMBER;
    last_button_event = BTN_EVENT_NONE;
}

const char *button_id_to_str(btn_id_e id)
{
    if (self.list != NULL) {
        for (uint8_t i = 0; i < self.num; i++) {
            if ((self.list[i].id == id) && (self.list[i].name != NULL)) {
                return self.list[i].name;
            }
        }
    }

    switch (id) {
        case BTN_ID_UP:
            return "上";
        case BTN_ID_DOWN:
            return "下";
        case BTN_ID_LEFT:
            return "左";
        case BTN_ID_RIGHT:
            return "右";
        case BTN_ID_CONFIRM:
            return "确认";
        case BTN_ID_BACK:
            return "返回";
        default:
            return "UNKNOWN";
    }
}

const char *button_event_to_str(btn_event_e event)
{
    switch (event) {
        case BTN_EVENT_NONE:
            return "NONE";
        case BTN_EVENT_PRESS_DOWN:
            return "PRESS_DOWN";
        case BTN_EVENT_PRESS_UP:
            return "PRESS_UP";
        case BTN_EVENT_SINGLE_CLICK:
            return "SINGLE_CLICK";
        case BTN_EVENT_DOUBLE_CLICK:
            return "DOUBLE_CLICK";
        case BTN_EVENT_REPEAT_CLICK:
            return "REPEAT_CLICK";
        case BTN_EVENT_LONG_PRESS:
            return "LONG_PRESS";
        case BTN_EVENT_LONG_HOLD:
            return "LONG_HOLD";
        case BTN_EVENT_LONG_HOLD_UP:
            return "LONG_HOLD_UP";
        default:
            return "UNKNOWN";
    }
}

void button_log_notify(btn_id_e id, const char *name, btn_permission_e permission, btn_event_e event)
{
    (void)permission;

    if (event == BTN_EVENT_SINGLE_CLICK || event == BTN_EVENT_DOUBLE_CLICK || event == BTN_EVENT_REPEAT_CLICK ||
        event == BTN_EVENT_LONG_PRESS || event == BTN_EVENT_LONG_HOLD || event == BTN_EVENT_LONG_HOLD_UP) {
#if defined(BUTTON_USE_LOG) && (BUTTON_USE_LOG)
        ESP_LOGI("button", "id=%d(%s) name=%s event=%d(%s)", (int)id, button_id_to_str(id),
                 (name != NULL) ? name : "NULL", (int)event, button_event_to_str(event));
#endif
    }
}

uint8_t button_get_level(btn_id_e id)
{
    if (id >= BTN_ID_MAX_NUMBER || self.list == NULL) {
        return 0xFF;
    }

    for (uint8_t i = 0; i < self.num; i++) {
        button_list_t *p = &self.list[i];
        if (p->id == id) {
            return button_read_gpio(p->gpio);
        }
    }

    return 0xFF;
}
