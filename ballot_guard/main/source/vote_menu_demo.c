/**
 * @file vote_menu_demo.c
 * @brief LCD UI 任务：业务屏 + 管理员菜单（对齐评审稿 v0.2）。
 */

#include "vote_menu_demo.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "buzzer.h"
#include "device_profile.h"
#include "log.h"
#include "menu.h"
#include "vote_menu_lcd.h"
#include "vote_menu_pages.h"
#include "vote_status.h"
#include "vote_history.h"

#define VOTE_MENU_TASK_STACK (4096U)
#define VOTE_MENU_TASK_PRIO (4U)
#define VOTE_MENU_TICK_MS (200U)
#define VOTE_MENU_CLOCK_MS (1000U)
#define VOTE_MENU_TOAST_MS (1000U)
#define VOTE_MENU_LCD_LOCK_MS (3000U)

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lcd_mtx;
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

static bool long_confirm_allowed(void)
{
    if (s_menu_mode) {
        return false;
    }
    return (s_screen == VOTE_LCD_SCREEN_HOME || s_screen == VOTE_LCD_SCREEN_LOCKED);
}

static void vote_menu_request_redraw(void)
{
    s_dirty_full = true;
    if (s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
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
        if (id == BTN_ID_CONFIRM) {
            if (event == BTN_EVENT_SINGLE_CLICK) {
                return MENU_EVT_ENTER;
            }
            if (event == BTN_EVENT_LONG_PRESS) {
                if (select_screen) {
                    return MENU_EVT_BACK;
                }
                if (long_confirm_allowed()) {
                    return MENU_EVT_ENTER;
                }
            }
        }
        if (id == BTN_ID_BACK && event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_BACK;
        }
        return MENU_EVT_HOME;
    }

    if (id == BTN_ID_LEFT) {
        if (event == BTN_EVENT_LONG_PRESS) {
            return MENU_EVT_ENTER;
        }
        if (event == BTN_EVENT_SINGLE_CLICK) {
            return select_screen ? MENU_EVT_LEFT : MENU_EVT_UP;
        }
    }
    if (id == BTN_ID_RIGHT) {
        if (event == BTN_EVENT_LONG_PRESS) {
            return MENU_EVT_BACK;
        }
        if (event == BTN_EVENT_SINGLE_CLICK) {
            return select_screen ? MENU_EVT_RIGHT : MENU_EVT_DOWN;
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
    s_menu_mode = true;
    menu_engine_reset(vote_menu_engine());
}

static void exit_menu_mode(void)
{
    s_menu_mode = false;
    s_screen    = screen_for_current_phase();
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
            (void)vote_status_add_valid(s_select_idx);
            business_nav_to(VOTE_LCD_SCREEN_COOLDOWN);
            s_back_screen = VOTE_LCD_SCREEN_VOTING;
            {
                vote_menu_settings_t *st = vote_menu_settings();
                if (st != NULL) {
                    s_cooldown_sec = st->cooldown_sec;
                }
            }
            return true;
        }
        if (evt == MENU_EVT_BACK) {
            (void)vote_status_add_spoiled();
            business_nav_to(VOTE_LCD_SCREEN_COOLDOWN);
            s_back_screen = VOTE_LCD_SCREEN_VOTING;
            {
                vote_menu_settings_t *st = vote_menu_settings();
                if (st != NULL) {
                    s_cooldown_sec = st->cooldown_sec;
                }
            }
            return true;
        }
        break;
    case VOTE_LCD_SCREEN_COOLDOWN:
    case VOTE_LCD_SCREEN_VIOLATION:
        if (evt == MENU_EVT_BACK) {
            return business_nav_back();
        }
        break;
    case VOTE_LCD_SCREEN_HISTORY:
        if (evt == MENU_EVT_UP) {
            if (s_history_idx > 0U) {
                s_history_idx--;
            }
            return true;
        }
        if (evt == MENU_EVT_DOWN) {
            const uint8_t max_idx =
                (vote_history_count() > 0U) ? (uint8_t)(vote_history_count() - 1U) : 0U;
            if (s_history_idx < max_idx) {
                s_history_idx++;
            }
            return true;
        }
        if (evt == MENU_EVT_BACK || evt == MENU_EVT_ENTER) {
            return business_nav_back();
        }
        break;
    default:
        break;
    }
    return false;
}

static void on_leave_app(void *ctx)
{
    (void)ctx;
    s_menu_mode = false;
    s_screen    = screen_for_current_phase();
    vote_menu_request_redraw();
}

static void on_ui_notify(void *ctx, const char *msg, int is_error)
{
    (void)ctx;
    if (msg == NULL) {
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

static void poll_phase_and_archive(void)
{
    int cd = 0;
    const char *phase = vote_status_current_phase(&cd);

    if (phase == NULL) {
        return;
    }
    notify_phase_buzzer(phase);
    if (strcmp(phase, "locked") == 0 && strcmp(s_last_phase, "locked") != 0) {
        (void)vote_history_archive_session_if_needed();
        s_locked_scroll_idx = 0U;
        if (!s_menu_mode && s_screen != VOTE_LCD_SCREEN_HISTORY) {
            business_nav_to(VOTE_LCD_SCREEN_LOCKED);
        }
    }
    (void)snprintf(s_last_phase, sizeof(s_last_phase), "%s", phase);
}

static void vote_menu_task(void *arg)
{
    (void)arg;

    for (;;) {
        const TickType_t now = xTaskGetTickCount();

        if (s_toast_until != 0U && now >= s_toast_until) {
            s_toast_until = 0U;
            s_dirty_full  = true;
        }
        if (now >= s_clock_redraw_at) {
            s_clock_redraw_at = now + pdMS_TO_TICKS(VOTE_MENU_CLOCK_MS);
            s_dirty           = true;
            poll_phase_and_archive();
        }

        if (s_dirty_full) {
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

    s_active          = true;
    s_screen          = VOTE_LCD_SCREEN_ADMIN;
    s_back_screen     = VOTE_LCD_SCREEN_HOME;
    s_menu_mode       = true;
    s_select_idx      = 0U;
    s_history_idx     = 0U;
    s_home_scroll_idx   = 0U;
    s_locked_scroll_idx = 0U;
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

    return STATUS_OK;
}

bool vote_menu_demo_is_active(void)
{
    return s_active;
}

bool vote_menu_demo_on_button(btn_id_e id, btn_event_e event)
{
    menu_engine_t *eng;
    menu_evt_t evt;
    menu_result_t r;

    if (!s_active) {
        return false;
    }

    evt = map_button(id, event);
    if (evt == MENU_EVT_HOME) {
        return false;
    }

    if (!s_menu_mode) {
        if (dispatch_business(evt)) {
            vote_menu_request_redraw();
            return true;
        }
        if (evt == MENU_EVT_ENTER && long_confirm_allowed()) {
            enter_menu_mode();
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
        LOG_INFO("vote_menu_demo: exit menu -> phase screen");
    }
    vote_menu_request_redraw();
    return true;
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
        }
        if (!s_menu_mode) {
            s_back_screen = s_screen;
        } else {
            s_back_screen = screen_for_current_phase();
        }
        s_menu_mode = false;
        s_screen    = screen;
        if (screen == VOTE_LCD_SCREEN_VIOLATION &&
            device_profile_platform_wants(DEVICE_PLATFORM_MASK_BUZZER) && buzzer_is_ready()) {
            (void)buzzer_play_pattern(BUZZER_PATTERN_ALARM);
        }
        if (screen == VOTE_LCD_SCREEN_SELECT && vote_status_candidate_count() > 0U) {
            s_select_idx = 0U;
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
        if (evt == MENU_EVT_ENTER && long_confirm_allowed()) {
            enter_menu_mode();
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
