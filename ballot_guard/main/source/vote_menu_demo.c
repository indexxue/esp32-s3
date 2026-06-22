/**
 * @file vote_menu_demo.c
 * @brief LCD UI 任务：业务屏 + 管理员菜单（对齐评审稿 v0.2）。
 */

#include "vote_menu_demo.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board.h"
#include "buzzer.h"
#include "button.h"
#include "device_profile.h"
#include "vote_led.h"
#include "log.h"
#include "menu.h"
#include "vote_menu_lcd.h"
#include "vote_menu_pages.h"
#include "vote_status.h"
#include "vote_history.h"
#include "vote_menu_zh.h"

#define VOTE_MENU_TASK_STACK (8192U)
#define VOTE_MENU_TASK_PRIO (4U)
#define VOTE_MENU_TICK_MS (200U)
#define VOTE_MENU_CLOCK_MS (1000U)
#define VOTE_MENU_TOAST_MS (1000U)
#define VOTE_MENU_LCD_LOCK_MS (3000U)
#define VOTE_BTN_QUEUE_LEN (16U)
#define VOTE_KEY_BEEP_MIN_MS (120U)

typedef struct {
    btn_id_e id;
    btn_event_e event;
} vote_btn_msg_t;

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lcd_mtx;
static QueueHandle_t s_btn_queue;
static TickType_t s_last_key_beep_tick;
static volatile bool s_active;
static volatile bool s_dirty;
static volatile bool s_dirty_full;
static char s_toast_msg[40];
static TickType_t s_toast_until;
static TickType_t s_clock_redraw_at;
static int s_toast_error;

static vote_lcd_screen_id_t s_screen      = VOTE_LCD_SCREEN_ADMIN;
static vote_lcd_screen_id_t s_back_screen = VOTE_LCD_SCREEN_HOME;
static bool s_menu_mode                   = true;
static uint8_t s_select_idx;
static uint8_t s_history_idx;
static uint8_t s_home_scroll_idx;
static uint8_t s_locked_scroll_idx;
static uint8_t s_cooldown_sec = 5U;
static char s_last_phase[16]    = "idle";
static bool s_select_navigated;
static vote_spoiled_type_e s_last_spoiled_display;
/** 允许下一次 HIGH→LOW 进入选人；投票/冷却期间为 false，冷却结束后置 true。 */
static bool s_ir_trigger_armed;
static TickType_t s_select_deadline;
static TickType_t s_confirm_down_since;
static bool s_confirm_spoiled_fired;
static TickType_t s_spoiled_alarm_until;

static void begin_cooldown_after_vote(void);

static vote_lcd_screen_id_t menu_page_to_screen(vote_menu_page_id_t page)
{
    switch (page) {
    case VOTE_MENU_PAGE_TIME:
        return VOTE_LCD_SCREEN_ADMIN_TIME;
    case VOTE_MENU_PAGE_COUNT:
        return VOTE_LCD_SCREEN_ADMIN_COUNT;
    case VOTE_MENU_PAGE_COOLDOWN:
        return VOTE_LCD_SCREEN_ADMIN_COOLDOWN;
    case VOTE_MENU_PAGE_RESET:
        return VOTE_LCD_SCREEN_ADMIN_RESET;
    case VOTE_MENU_PAGE_ENTER:
        return VOTE_LCD_SCREEN_ADMIN_ENTER;
    case VOTE_MENU_PAGE_CLOCK:
        return VOTE_LCD_SCREEN_ADMIN_CLOCK;
    default:
        return VOTE_LCD_SCREEN_ADMIN;
    }
}

static vote_lcd_screen_id_t screen_for_current_phase(void)
{
    int cd = 0;
    const char *phase = vote_status_current_phase(&cd);
    return vote_status_lcd_screen_for_phase(phase);
}

static bool phase_is_voting(void)
{
    const char *phase = vote_status_current_phase(NULL);
    return (phase != NULL && strcmp(phase, "voting") == 0);
}

static bool vote_menu_six_keys(void);

static void clear_select_session(void);
static void enter_menu_mode(void);
static void vote_menu_request_redraw(void);

static btn_id_e admin_back_button(void)
{
    return vote_menu_six_keys() ? BTN_ID_BACK : BTN_ID_RIGHT;
}

static bool admin_back_enter_allowed(void)
{
    return s_active && !s_menu_mode;
}

static bool try_enter_admin_via_back_long(btn_id_e id, btn_event_e event)
{
    if (!admin_back_enter_allowed()) {
        return false;
    }
    if (id != admin_back_button() || event != BTN_EVENT_LONG_PRESS) {
        return false;
    }

    if (s_screen == VOTE_LCD_SCREEN_SELECT) {
        clear_select_session();
        s_ir_trigger_armed = true;
    }
    enter_menu_mode();
    vote_menu_request_redraw();
    LOG_INFO("vote_menu_demo: back long %ums -> ADMIN", (unsigned)VOTE_ADMIN_BACK_HOLD_MS);
    return true;
}

static void vote_menu_request_redraw(void)
{
    s_dirty_full = true;
    if (s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
    }
}

/** 仅唤醒 UI 任务处理按键队列，不标记整屏重绘（避免先刷旧画面再跳转）。 */
static void vote_menu_wake_task(void)
{
    if (s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
    }
}

static void show_toast(const char *msg, int is_error)
{
    if (msg == NULL || msg[0] == '\0') {
        return;
    }
    if (s_lcd_mtx != NULL && xSemaphoreTake(s_lcd_mtx, pdMS_TO_TICKS(VOTE_MENU_LCD_LOCK_MS)) == pdTRUE) {
        (void)snprintf(s_toast_msg, sizeof(s_toast_msg), "%s", msg);
        s_toast_error = is_error;
        s_toast_until = xTaskGetTickCount() + pdMS_TO_TICKS(VOTE_MENU_TOAST_MS);
        xSemaphoreGive(s_lcd_mtx);
    }
    vote_menu_request_redraw();
}

static void vote_menu_key_beep(void)
{
    const TickType_t now = xTaskGetTickCount();

    if (!device_profile_platform_wants(DEVICE_PLATFORM_MASK_BUZZER) || !buzzer_is_ready()) {
        return;
    }
    if (s_spoiled_alarm_until != 0U && now < s_spoiled_alarm_until) {
        return;
    }
    if ((now - s_last_key_beep_tick) < pdMS_TO_TICKS(VOTE_KEY_BEEP_MIN_MS)) {
        return;
    }
    if (buzzer_pattern_busy()) {
        return;
    }
    s_last_key_beep_tick = now;
    (void)buzzer_play_pattern(BUZZER_PATTERN_SHORT);
}

static void start_spoiled_alarm(void)
{
    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_BUZZER) && buzzer_is_ready()) {
        (void)buzzer_play_pattern(BUZZER_PATTERN_ALARM);
    }
    s_spoiled_alarm_until = xTaskGetTickCount() + pdMS_TO_TICKS(VOTE_SPOILED_ALARM_MS);
}

static void stop_spoiled_alarm(void)
{
    if (s_spoiled_alarm_until == 0U) {
        return;
    }
    s_spoiled_alarm_until = 0U;
    if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_BUZZER)) {
        buzzer_stop_pattern();
    }
}

static void poll_spoiled_alarm(void)
{
    if (s_spoiled_alarm_until == 0U) {
        return;
    }
    if ((int32_t)(xTaskGetTickCount() - s_spoiled_alarm_until) >= 0) {
        stop_spoiled_alarm();
    }
}

static void show_spoiled_toast(vote_spoiled_type_e stype)
{
    char buf[40];
    const char *detail;

    switch (stype) {
    case VOTE_SPOILED_MULTIPLE:
        detail = VOTE_ZH_MULTIPLE;
        break;
    case VOTE_SPOILED_IRREGULAR:
        detail = VOTE_ZH_IRREGULAR;
        break;
    default:
        detail = VOTE_ZH_BLANK;
        break;
    }
    (void)snprintf(buf, sizeof(buf), "%s %s", VOTE_ZH_SPOILED, detail);
    show_toast(buf, 1);
}

static btn_id_e select_confirm_button(void)
{
    return (device_profile_button_count() >= 6U) ? BTN_ID_CONFIRM : BTN_ID_LEFT;
}

static bool is_button_pressed(btn_id_e id)
{
    const uint8_t lv = button_get_level(id);
    const uint8_t n  = device_profile_button_count();
    uint8_t i;

    if (lv == 0xFFU) {
        return false;
    }
    for (i = 0U; i < n; i++) {
        const device_button_spec_t *spec = device_profile_button_spec(i);
        if (spec != NULL && spec->id == id) {
            return (lv == spec->active_level);
        }
    }
    return false;
}

static void clear_select_session(void)
{
    s_select_deadline          = 0;
    s_confirm_down_since       = 0;
    s_confirm_spoiled_fired    = false;
}

static void arm_select_session(void)
{
    s_select_deadline          = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)VOTE_SELECT_TIMEOUT_SEC * 1000U);
    s_confirm_down_since       = 0;
    s_confirm_spoiled_fired    = false;
}

static uint8_t select_timeout_remaining_sec(void)
{
    TickType_t rem;

    if (s_screen != VOTE_LCD_SCREEN_SELECT || s_select_deadline == 0U) {
        return 0U;
    }
    rem = s_select_deadline - xTaskGetTickCount();
    if ((int32_t)rem <= 0) {
        return 0U;
    }
    return (uint8_t)(((uint32_t)rem + pdMS_TO_TICKS(999U)) / pdMS_TO_TICKS(1000U));
}

static void finish_spoiled_vote(vote_spoiled_type_e stype)
{
    if (s_confirm_spoiled_fired) {
        return;
    }
    s_confirm_spoiled_fired    = true;
    s_select_deadline          = 0;
    s_confirm_down_since       = 0;
    (void)vote_status_add_spoiled_typed(stype, s_select_idx);
    s_last_spoiled_display = stype;
    show_spoiled_toast(stype);
    vote_led_on_spoiled_vote();
    start_spoiled_alarm();
    begin_cooldown_after_vote();
    vote_menu_request_redraw();
}

static void finish_valid_vote(void)
{
    clear_select_session();
    (void)vote_status_add_valid(s_select_idx);
    s_last_spoiled_display = VOTE_SPOILED_NONE;
    vote_led_on_valid_vote();
    begin_cooldown_after_vote();
}

static void poll_select_session(void)
{
    const TickType_t now = xTaskGetTickCount();

    if (s_menu_mode || s_screen != VOTE_LCD_SCREEN_SELECT || s_confirm_spoiled_fired) {
        return;
    }

    if (s_select_deadline != 0U && (int32_t)(now - s_select_deadline) >= 0) {
        finish_spoiled_vote(VOTE_SPOILED_IRREGULAR);
        return;
    }

    if (s_confirm_down_since != 0U) {
        if (!is_button_pressed(select_confirm_button())) {
            s_confirm_down_since = 0;
            return;
        }
        if ((now - s_confirm_down_since) >= pdMS_TO_TICKS(VOTE_SELECT_SPOILED_HOLD_MS)) {
            vote_spoiled_type_e stype =
                s_select_navigated ? VOTE_SPOILED_MULTIPLE : VOTE_SPOILED_BLANK;
            finish_spoiled_vote(stype);
        }
    }
}

static vote_lcd_ctx_t build_draw_ctx(void)
{
    vote_lcd_ctx_t ctx = {
        .screen          = s_menu_mode ? menu_page_to_screen(vote_menu_page_id_of(menu_current_page(vote_menu_engine())))
                                      : s_screen,
        .menu_eng        = vote_menu_engine(),
        .select_idx      = s_select_idx,
        .history_idx     = s_history_idx,
        .home_scroll_idx   = s_home_scroll_idx,
        .locked_scroll_idx = s_locked_scroll_idx,
        .cooldown_sec    = s_cooldown_sec,
        .select_timeout_sec = select_timeout_remaining_sec(),
        .spoiled_type    = s_last_spoiled_display,
    };

    if (s_menu_mode) {
        ctx.screen = menu_page_to_screen(vote_menu_page_id_of(menu_current_page(vote_menu_engine())));
    }
    return ctx;
}

static void redraw_full(void)
{
    st7789_t *lcd = BoardSt7789();
    vote_lcd_ctx_t ctx;

    if (lcd == NULL || s_lcd_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lcd_mtx, pdMS_TO_TICKS(VOTE_MENU_LCD_LOCK_MS)) != pdTRUE) {
        LOG_WARN("vote_menu_demo: LCD lock timeout");
        return;
    }

    ctx = build_draw_ctx();
    vote_lcd_draw(lcd, &ctx);
    if (s_toast_until != 0U && xTaskGetTickCount() < s_toast_until) {
        vote_menu_lcd_draw_toast(lcd, s_toast_msg, s_toast_error);
    }

    xSemaphoreGive(s_lcd_mtx);
}

static void redraw_clock_only(void)
{
    st7789_t *lcd = BoardSt7789();
    vote_lcd_ctx_t ctx;

    if (lcd == NULL || s_lcd_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lcd_mtx, pdMS_TO_TICKS(VOTE_MENU_LCD_LOCK_MS)) != pdTRUE) {
        return;
    }
    ctx = build_draw_ctx();
    vote_lcd_draw_clock(lcd, &ctx);
    xSemaphoreGive(s_lcd_mtx);
}

static bool vote_menu_six_keys(void)
{
    return device_profile_button_count() >= 6U;
}

/** 开发板 2 键：左=上/长按确认，右=下/长按返回；选人屏左/右=横向。产品 6 键时各键独立。 */
static menu_evt_t map_button(btn_id_e id, btn_event_e event)
{
    const bool select_screen = (!s_menu_mode && s_screen == VOTE_LCD_SCREEN_SELECT);

    if (vote_menu_six_keys()) {
        if (id == BTN_ID_UP && event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_UP;
        }
        if (id == BTN_ID_DOWN && event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_DOWN;
        }
        if (id == BTN_ID_LEFT && event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_LEFT;
        }
        if (id == BTN_ID_RIGHT && event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_RIGHT;
        }
        if (id == BTN_ID_CONFIRM && event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_ENTER;
        }
        if (id == BTN_ID_BACK && event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_BACK;
        }
        return MENU_EVT_HOME;
    }

    if (id == BTN_ID_LEFT) {
        if (select_screen) {
            if (event == BTN_EVENT_SINGLE_CLICK) {
                return MENU_EVT_LEFT;
            }
            return MENU_EVT_HOME;
        }
        if (event == BTN_EVENT_LONG_PRESS) {
            return MENU_EVT_ENTER;
        }
        if (event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_UP;
        }
    }
    if (id == BTN_ID_RIGHT) {
        if (select_screen) {
            if (event == BTN_EVENT_SINGLE_CLICK) {
                return MENU_EVT_ENTER;
            }
            return MENU_EVT_HOME;
        }
        if (event == BTN_EVENT_LONG_PRESS) {
            return MENU_EVT_BACK;
        }
        if (event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_DOWN;
        }
    }
    if (id == BTN_ID_UP && event == BTN_EVENT_SINGLE_CLICK) {
        return MENU_EVT_UP;
    }
    if (id == BTN_ID_DOWN && event == BTN_EVENT_SINGLE_CLICK) {
        return MENU_EVT_DOWN;
    }
    return MENU_EVT_HOME;
}

static void enter_menu_mode(void)
{
    s_menu_mode        = true;
    s_ir_trigger_armed = false;
    menu_engine_reset(vote_menu_engine());
}

static void exit_menu_mode(void)
{
    s_menu_mode = false;
    s_screen    = screen_for_current_phase();
    if (vote_status_cooldown_remaining() == 0U) {
        s_ir_trigger_armed = true;
    }
}

static void business_nav_to(vote_lcd_screen_id_t screen)
{
    if (!s_menu_mode) {
        s_back_screen = s_screen;
    } else {
        s_back_screen = screen_for_current_phase();
    }
    s_screen = screen;
}

static bool business_nav_back(void)
{
    vote_lcd_screen_id_t target = s_back_screen;

    if (vote_lcd_screen_is_menu(target)) {
        target = screen_for_current_phase();
    }
    if (target == s_screen) {
        target = screen_for_current_phase();
    }
    s_screen = target;
    return true;
}

bool vote_menu_demo_enter_voting(void)
{
    if (!s_active) {
        return false;
    }
    exit_menu_mode();
    vote_menu_request_redraw();
    return true;
}

static void begin_cooldown_after_vote(void)
{
    vote_menu_settings_t *st = vote_menu_settings();
    uint8_t cd               = 5U;

    s_ir_trigger_armed = false;
    if (st != NULL) {
        cd = st->cooldown_sec;
    }
    s_cooldown_sec = cd;
    vote_status_set_cooldown_remaining(cd);
    business_nav_to(VOTE_LCD_SCREEN_HOME);
    s_back_screen = VOTE_LCD_SCREEN_VOTING;
}

static bool dispatch_business(menu_evt_t evt)
{
    uint8_t count = vote_status_candidate_count();

    switch (s_screen) {
    case VOTE_LCD_SCREEN_HOME:
        if (evt == MENU_EVT_UP) {
            s_history_idx = 0U;
            business_nav_to(VOTE_LCD_SCREEN_HISTORY);
            return true;
        }
        if (evt == MENU_EVT_DOWN) {
            if (count == 0U) {
                count = 1U;
            }
            s_home_scroll_idx = (uint8_t)((s_home_scroll_idx + 1U) % count);
            return true;
        }
        if (evt == MENU_EVT_BACK && phase_is_voting()) {
            business_nav_to(VOTE_LCD_SCREEN_VOTING);
            return true;
        }
        break;
    case VOTE_LCD_SCREEN_LOCKED:
        if (evt == MENU_EVT_UP) {
            if (s_locked_scroll_idx > 0U) {
                s_locked_scroll_idx--;
            }
            return true;
        }
        if (evt == MENU_EVT_DOWN) {
            if (count == 0U) {
                count = 1U;
            }
            if (s_locked_scroll_idx + 1U < count) {
                s_locked_scroll_idx++;
            }
            return true;
        }
        if (evt == MENU_EVT_BACK) {
            return business_nav_back();
        }
        break;
    case VOTE_LCD_SCREEN_SELECT:
        if (evt == MENU_EVT_LEFT || evt == MENU_EVT_RIGHT || evt == MENU_EVT_UP || evt == MENU_EVT_DOWN) {
            s_select_navigated = true;
        }
        if (evt == MENU_EVT_LEFT) {
            if (s_select_idx > 0U) {
                s_select_idx--;
            }
            return true;
        }
        if (evt == MENU_EVT_RIGHT) {
            if (count == 0U) {
                count = 1U;
            }
            if (s_select_idx + 1U < count) {
                s_select_idx++;
            }
            return true;
        }
        if (evt == MENU_EVT_ENTER) {
            if (s_confirm_spoiled_fired) {
                return true;
            }
            finish_valid_vote();
            return true;
        }
        if (evt == MENU_EVT_BACK) {
            clear_select_session();
            s_ir_trigger_armed = true;
            business_nav_to(VOTE_LCD_SCREEN_VOTING);
            return true;
        }
        break;
    case VOTE_LCD_SCREEN_VOTING:
        if (evt == MENU_EVT_BACK) {
            business_nav_to(VOTE_LCD_SCREEN_HOME);
            return true;
        }
        break;
    case VOTE_LCD_SCREEN_COOLDOWN:
        if (evt == MENU_EVT_BACK) {
            return business_nav_back();
        }
        break;
    case VOTE_LCD_SCREEN_VIOLATION:
        if (evt == MENU_EVT_BACK) {
            buzzer_stop_pattern();
            vote_led_on_violation_end();
            return business_nav_back();
        }
        break;
    case VOTE_LCD_SCREEN_HISTORY:
    {
        const uint8_t total = vote_history_count();

        if (evt == MENU_EVT_UP) {
            if (s_history_idx > 0U) {
                s_history_idx--;
            }
            return true;
        }
        if (evt == MENU_EVT_DOWN) {
            if (s_history_idx + 1U < total) {
                s_history_idx++;
            }
            return true;
        }
        if (evt == MENU_EVT_BACK || evt == MENU_EVT_ENTER) {
            return business_nav_back();
        }
        break;
    }
    default:
        break;
    }
    return false;
}

static void on_leave_app(void *ctx)
{
    (void)ctx;
    exit_menu_mode();
    vote_menu_request_redraw();
}

static void on_ui_notify(void *ctx, const char *msg, int is_error)
{
    (void)ctx;
    show_toast(msg, is_error);
}

static void notify_phase_led(const char *phase)
{
    if (phase == NULL) {
        return;
    }
    if (strcmp(phase, s_last_phase) == 0) {
        return;
    }
    vote_led_sync_phase(phase);
}

static void notify_phase_buzzer(const char *phase)
{
    if (!device_profile_platform_wants(DEVICE_PLATFORM_MASK_BUZZER) || !buzzer_is_ready()) {
        return;
    }
    if (phase == NULL) {
        return;
    }
    if (strcmp(phase, "voting") == 0 && strcmp(s_last_phase, "voting") != 0) {
        (void)buzzer_play_pattern(BUZZER_PATTERN_DOUBLE_SHORT);
    } else if (strcmp(phase, "locked") == 0 && strcmp(s_last_phase, "locked") != 0) {
        (void)buzzer_play_pattern(BUZZER_PATTERN_TRIPLE_LONG);
    }
}

static void poll_cooldown_tick(void)
{
    uint8_t rem = vote_status_cooldown_remaining();

    if (rem == 0U) {
        return;
    }
    rem = (uint8_t)(rem - 1U);
    vote_status_set_cooldown_remaining(rem);
    s_cooldown_sec = rem;
    s_dirty        = true;
    if (rem == 0U && !s_menu_mode) {
        s_ir_trigger_armed = true;
        if (s_screen == VOTE_LCD_SCREEN_HOME || s_screen == VOTE_LCD_SCREEN_COOLDOWN) {
            business_nav_to(VOTE_LCD_SCREEN_VOTING);
            s_dirty_full = true;
        }
    }
}

static void poll_phase_and_archive(void)
{
    int cd = 0;
    const char *phase = vote_status_current_phase(&cd);

    if (phase == NULL) {
        return;
    }
    notify_phase_buzzer(phase);
    notify_phase_led(phase);
    if (strcmp(phase, "voting") == 0 && strcmp(s_last_phase, "voting") != 0) {
        if (!s_menu_mode && vote_status_cooldown_remaining() == 0U) {
            s_ir_trigger_armed = true;
        }
        if (!s_menu_mode && s_screen != VOTE_LCD_SCREEN_SELECT && s_screen != VOTE_LCD_SCREEN_COOLDOWN &&
            s_screen != VOTE_LCD_SCREEN_VIOLATION) {
            business_nav_to(VOTE_LCD_SCREEN_VOTING);
            s_dirty_full = true;
        }
    }
    if (strcmp(phase, "locked") == 0 && strcmp(s_last_phase, "locked") != 0) {
        (void)vote_history_archive_session_if_needed();
        s_locked_scroll_idx = 0U;
        vote_status_set_cooldown_remaining(0U);
        if (!s_menu_mode && s_screen != VOTE_LCD_SCREEN_HISTORY) {
            business_nav_to(VOTE_LCD_SCREEN_LOCKED);
            s_dirty_full = true;
        }
    }
    if (strcmp(phase, "waiting") == 0 && strcmp(s_last_phase, "waiting") != 0) {
        vote_status_set_cooldown_remaining(0U);
        if (!s_menu_mode && s_screen != VOTE_LCD_SCREEN_HOME && s_screen != VOTE_LCD_SCREEN_HISTORY) {
            business_nav_to(VOTE_LCD_SCREEN_HOME);
            s_dirty_full = true;
        }
    }
    poll_cooldown_tick();
    (void)snprintf(s_last_phase, sizeof(s_last_phase), "%s", phase);
}

static bool ir_level_monitor_enabled(void)
{
    const char *phase;

    if (!s_active || s_menu_mode) {
        return false;
    }
    if (vote_status_cooldown_remaining() > 0U) {
        return false;
    }
    phase = vote_status_current_phase(NULL);
    return (phase != NULL && strcmp(phase, "voting") == 0);
}

static bool vote_menu_demo_on_ir_violation(void)
{
    const char *phase;

    if (!s_active || s_menu_mode) {
        return false;
    }
    if (vote_status_cooldown_remaining() == 0U) {
        return false;
    }
    phase = vote_status_current_phase(NULL);
    if (phase == NULL || strcmp(phase, "voting") != 0) {
        return false;
    }
    (void)vote_menu_demo_goto_screen(VOTE_LCD_SCREEN_VIOLATION);
    return true;
}

static bool vote_menu_demo_on_ir_approach(void)
{
    if (!ir_level_monitor_enabled()) {
        return false;
    }

    if (!s_ir_trigger_armed) {
        return false;
    }

    if (s_screen == VOTE_LCD_SCREEN_VOTING || s_screen == VOTE_LCD_SCREEN_HOME) {
        s_ir_trigger_armed   = false;
        s_select_navigated   = false;
        s_select_idx         = 0U;
        vote_led_on_ir_approach();
        (void)vote_menu_demo_goto_screen(VOTE_LCD_SCREEN_SELECT);
        return true;
    }

    return false;
}

bool vote_menu_demo_on_ir_level(board_ir_channel_e channel, u32_t prev_level, u32_t new_level)
{
    (void)channel;
    if (prev_level == 1U && new_level == 0U) {
        if (vote_menu_demo_on_ir_violation()) {
            return true;
        }
    }
    if (!ir_level_monitor_enabled()) {
        return false;
    }
    /* 靠近：HIGH->LOW（上拉空闲为高，靠近对地）；离开 LOW->HIGH 忽略。 */
    if (prev_level == 1U && new_level == 0U) {
        return vote_menu_demo_on_ir_approach();
    }
    return false;
}

bool vote_menu_demo_on_ir(void)
{
    return vote_menu_demo_on_ir_approach();
}

static void vote_menu_handle_button(btn_id_e id, btn_event_e event);

static void poll_button_queue(void)
{
    vote_btn_msg_t msg;

    if (s_btn_queue == NULL) {
        return;
    }

    while (xQueueReceive(s_btn_queue, &msg, 0) == pdTRUE) {
        vote_menu_handle_button(msg.id, msg.event);
    }
}

static void vote_menu_task(void *arg)
{
    (void)arg;

    for (;;) {
        const TickType_t now = xTaskGetTickCount();

        poll_button_queue();

        if (s_toast_until != 0U && now >= s_toast_until) {
            s_toast_until = 0U;
            s_dirty_full  = true;
        }
        if (now >= s_clock_redraw_at) {
            s_clock_redraw_at = now + pdMS_TO_TICKS(VOTE_MENU_CLOCK_MS);
            s_dirty           = true;
            poll_phase_and_archive();
        }

        poll_select_session();
        poll_spoiled_alarm();

        if (s_dirty_full) {
            poll_button_queue();
            redraw_full();
            s_dirty_full = false;
            s_dirty      = false;
        } else if (s_dirty) {
            redraw_clock_only();
            s_dirty = false;
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(VOTE_MENU_TICK_MS));
    }
}

status_t vote_menu_demo_start(void)
{
    vote_menu_settings_t *st;

    if (!device_profile_board_wants(DEVICE_BOARD_MASK_LCD)) {
        return STATUS_OK;
    }
    if (BoardSt7789() == NULL) {
        LOG_WARN("vote_menu_demo: LCD not ready");
        return STATUS_FAIL;
    }

    vote_menu_pages_init();
    vote_menu_set_ui_notify(on_ui_notify, NULL);
    vote_menu_set_leave_app_cb(on_leave_app, NULL);

    st = vote_menu_settings();
    if (st != NULL) {
        s_cooldown_sec = st->cooldown_sec;
    }

    if (s_lcd_mtx == NULL) {
        s_lcd_mtx = xSemaphoreCreateMutex();
        if (s_lcd_mtx == NULL) {
            LOG_ERROR("vote_menu_demo: LCD mutex create failed");
            return STATUS_FAIL;
        }
    }

    if (s_btn_queue == NULL) {
        s_btn_queue = xQueueCreate(VOTE_BTN_QUEUE_LEN, sizeof(vote_btn_msg_t));
        if (s_btn_queue == NULL) {
            LOG_ERROR("vote_menu_demo: button queue create failed");
            return STATUS_FAIL;
        }
    }

    s_active          = true;
    s_screen          = VOTE_LCD_SCREEN_ADMIN;
    s_back_screen     = VOTE_LCD_SCREEN_HOME;
    s_menu_mode       = true;
    s_select_idx      = 0U;
    s_history_idx     = 0U;
    s_home_scroll_idx   = 0U;
    s_locked_scroll_idx = 0U;
    s_ir_trigger_armed  = false;
    s_dirty           = false;
    s_dirty_full      = true;
    s_toast_until     = 0U;
    s_clock_redraw_at = xTaskGetTickCount() + pdMS_TO_TICKS(VOTE_MENU_CLOCK_MS);

    if (s_task != NULL) {
        vote_menu_request_redraw();
        return STATUS_OK;
    }

    if (xTaskCreate(vote_menu_task, "vote_menu", VOTE_MENU_TASK_STACK, NULL, VOTE_MENU_TASK_PRIO, &s_task) != pdPASS) {
        LOG_ERROR("vote_menu_demo: create task failed");
        s_active = false;
        return STATUS_FAIL;
    }

    LOG_INFO("vote_menu_demo: boot -> ADMIN (%s)",
             vote_menu_six_keys() ? "6-key UP/DN/L/R/OK/BACK" : "2-key L=up Llong=OK R=down Rlong=back");

    {
        int cd = 0;
        const char *phase = vote_status_current_phase(&cd);
        if (phase != NULL && strcmp(phase, "locked") == 0) {
            (void)vote_history_archive_session_if_needed();
        }
        if (phase != NULL) {
            (void)snprintf(s_last_phase, sizeof(s_last_phase), "%s", phase);
        }
    }

    vote_led_on_boot_ready();

    return STATUS_OK;
}

bool vote_menu_demo_is_active(void)
{
    return s_active;
}

bool vote_menu_demo_on_button(btn_id_e id, btn_event_e event)
{
    vote_btn_msg_t msg;

    if (!s_active || s_btn_queue == NULL) {
        return false;
    }

    msg.id    = id;
    msg.event = event;
    if (xQueueSend(s_btn_queue, &msg, 0) != pdTRUE) {
        LOG_WARN("vote_menu_demo: button queue full, drop id=%d evt=%d", (int)id, (int)event);
        return false;
    }

    vote_menu_wake_task();
    return true;
}

static void vote_menu_handle_button(btn_id_e id, btn_event_e event)
{
    menu_engine_t *eng;
    menu_evt_t evt;
    menu_result_t r;

    if (!s_active) {
        return;
    }

    if (event == BTN_EVENT_PRESS_DOWN) {
        vote_menu_key_beep();
    }

    if (try_enter_admin_via_back_long(id, event)) {
        return;
    }

    if (!s_menu_mode && s_screen == VOTE_LCD_SCREEN_SELECT && event == BTN_EVENT_PRESS_DOWN &&
        id == select_confirm_button()) {
        s_confirm_down_since = xTaskGetTickCount();
    }

    evt = map_button(id, event);
    if (evt == MENU_EVT_HOME) {
        return;
    }

    if (!s_menu_mode) {
        if (dispatch_business(evt)) {
            vote_menu_request_redraw();
        }
        return;
    }

    eng = vote_menu_engine();
    r   = menu_dispatch(eng, evt);
    if (evt == MENU_EVT_ENTER && (r == MENU_RESULT_AT_BOUND || r == MENU_RESULT_IGNORED)) {
        if (vote_menu_pages_confirm_save(eng)) {
            vote_menu_request_redraw();
            return;
        }
    }
    if (r == MENU_RESULT_EXIT) {
        exit_menu_mode();
        LOG_INFO("vote_menu_demo: exit menu -> phase screen");
    }
    vote_menu_request_redraw();
}

vote_lcd_screen_id_t vote_menu_demo_current_screen(void)
{
    if (!s_active) {
        return VOTE_LCD_SCREEN_ADMIN;
    }
    if (s_menu_mode) {
        return menu_page_to_screen(vote_menu_page_id_of(menu_current_page(vote_menu_engine())));
    }
    return s_screen;
}

vote_menu_page_id_t vote_menu_demo_current_page(void)
{
    const vote_lcd_screen_id_t scr = vote_menu_demo_current_screen();
    if (!vote_lcd_screen_is_menu(scr)) {
        return VOTE_MENU_PAGE_ADMIN;
    }
    return (vote_menu_page_id_t)scr;
}

bool vote_menu_demo_goto_screen(vote_lcd_screen_id_t screen)
{
    menu_engine_t *eng;
    const menu_page_t *target;
    menu_result_t r;

    if (!s_active) {
        return false;
    }

    if (vote_lcd_screen_is_menu(screen)) {
        target = vote_menu_page_by_id((vote_menu_page_id_t)screen);
        if (target == NULL) {
            return false;
        }
        s_menu_mode = true;
        eng         = vote_menu_engine();
        r           = menu_nav_goto(eng, target);
        if (r == MENU_RESULT_ERROR) {
            return false;
        }
    } else {
        if (!s_menu_mode && s_screen == VOTE_LCD_SCREEN_VIOLATION && screen != VOTE_LCD_SCREEN_VIOLATION) {
            buzzer_stop_pattern();
            vote_led_on_violation_end();
        }
        if (!s_menu_mode) {
            s_back_screen = s_screen;
        } else {
            s_back_screen = screen_for_current_phase();
        }
        s_menu_mode = false;
        s_screen    = screen;
        if (screen == VOTE_LCD_SCREEN_VIOLATION) {
            vote_led_on_violation();
            if (device_profile_platform_wants(DEVICE_PLATFORM_MASK_BUZZER) && buzzer_is_ready()) {
                (void)buzzer_play_pattern(BUZZER_PATTERN_ALARM);
            }
        } else if (screen == VOTE_LCD_SCREEN_SELECT && vote_status_candidate_count() > 0U) {
            s_select_idx           = 0U;
            s_select_navigated     = false;
            s_last_spoiled_display = VOTE_SPOILED_NONE;
            arm_select_session();
        }
        if (screen == VOTE_LCD_SCREEN_LOCKED) {
            s_locked_scroll_idx = 0U;
        }
        if (screen == VOTE_LCD_SCREEN_COOLDOWN) {
            vote_menu_settings_t *st = vote_menu_settings();
            if (st != NULL) {
                s_cooldown_sec = st->cooldown_sec;
            }
        }
    }

    vote_menu_request_redraw();
    return true;
}

bool vote_menu_demo_goto_page(vote_menu_page_id_t page)
{
    return vote_menu_demo_goto_screen((vote_lcd_screen_id_t)page);
}

bool vote_menu_demo_dispatch(menu_evt_t evt)
{
    menu_engine_t *eng;
    menu_result_t r;

    if (!s_active) {
        return false;
    }
    if (evt == MENU_EVT_HOME) {
        return false;
    }

    if (!s_menu_mode) {
        if (dispatch_business(evt)) {
            vote_menu_request_redraw();
            return true;
        }
        return false;
    }

    eng = vote_menu_engine();
    r   = menu_dispatch(eng, evt);
    if (evt == MENU_EVT_ENTER && (r == MENU_RESULT_AT_BOUND || r == MENU_RESULT_IGNORED)) {
        if (vote_menu_pages_confirm_save(eng)) {
            vote_menu_request_redraw();
            return true;
        }
    }
    if (r == MENU_RESULT_EXIT) {
        exit_menu_mode();
    }
    vote_menu_request_redraw();
    return (r != MENU_RESULT_ERROR);
}
