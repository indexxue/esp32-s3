/**
 * @file vote_menu_demo.c
 * @brief 菜单演示任务：驱动 menu 引擎并刷新 LCD。
 */

#include "vote_menu_demo.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "device_profile.h"
#include "log.h"
#include "menu.h"
#include "vote_menu_lcd.h"
#include "vote_menu_pages.h"

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

static void vote_menu_request_redraw(void)
{
    s_dirty_full = true;
    if (s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
    }
}

static void redraw_full(void)
{
    st7789_t *lcd = BoardSt7789();
    menu_engine_t *eng;

    if (lcd == NULL || s_lcd_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lcd_mtx, pdMS_TO_TICKS(VOTE_MENU_LCD_LOCK_MS)) != pdTRUE) {
        LOG_WARN("vote_menu_demo: LCD lock timeout");
        return;
    }

    eng = vote_menu_engine();
    vote_menu_lcd_draw(lcd, eng);
    if (s_toast_until != 0U && xTaskGetTickCount() < s_toast_until) {
        vote_menu_lcd_draw_toast(lcd, s_toast_msg, s_toast_error);
    }

    xSemaphoreGive(s_lcd_mtx);
}

static void redraw_clock_only(void)
{
    st7789_t *lcd = BoardSt7789();

    if (lcd == NULL || s_lcd_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lcd_mtx, pdMS_TO_TICKS(VOTE_MENU_LCD_LOCK_MS)) != pdTRUE) {
        return;
    }
    vote_menu_lcd_draw_clock(lcd);
    xSemaphoreGive(s_lcd_mtx);
}

static menu_evt_t map_button(btn_id_e id, btn_event_e event)
{
    if (id == BTN_ID_LEFT) {
        if (event == BTN_EVENT_LONG_PRESS) {
            return MENU_EVT_ENTER;
        }
        if (event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_UP;
        }
    }
    if (id == BTN_ID_RIGHT) {
        if (event == BTN_EVENT_LONG_PRESS) {
            return MENU_EVT_BACK;
        }
        if (event == BTN_EVENT_SINGLE_CLICK) {
            return MENU_EVT_DOWN;
        }
    }
    return MENU_EVT_HOME; /* sentinel: ignored below */
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
    if (!device_profile_board_wants(DEVICE_BOARD_MASK_LCD)) {
        return STATUS_OK;
    }
    if (BoardSt7789() == NULL) {
        LOG_WARN("vote_menu_demo: LCD not ready");
        return STATUS_FAIL;
    }

    vote_menu_pages_init();
    vote_menu_set_ui_notify(on_ui_notify, NULL);

    if (s_lcd_mtx == NULL) {
        s_lcd_mtx = xSemaphoreCreateMutex();
        if (s_lcd_mtx == NULL) {
            LOG_ERROR("vote_menu_demo: LCD mutex create failed");
            return STATUS_FAIL;
        }
    }

    s_active          = true;
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

    LOG_INFO("vote_menu_demo: active (L=up/OK Llong=enter R=down Rlong=back)");
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

    eng = vote_menu_engine();
    r   = menu_dispatch(eng, evt);
    if (r == MENU_RESULT_EXIT) {
        LOG_INFO("vote_menu_demo: exit menu (still showing admin root)");
    }
    vote_menu_request_redraw();
    return true;
}
