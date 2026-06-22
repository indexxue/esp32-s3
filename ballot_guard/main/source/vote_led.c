/**
 * @file vote_led.c
 * @brief ballot_guard 投票业务 RGB 灯效映射（WS2812 ×3，GPIO48）。
 */

#include "vote_led.h"

#include <string.h>

#include "device_profile.h"
#include "led_scene.h"
#include "vote_status.h"

static bool s_fault_latched;
static bool s_wifi_offline;

static bool vote_led_enabled(void)
{
    return device_profile_platform_wants(DEVICE_PLATFORM_MASK_LED);
}

static void vote_led_cancel_base(void)
{
    (void)led_scene_cancel(LED_SCENE_ID_BALLOT_IDLE);
    (void)led_scene_cancel(LED_SCENE_ID_BALLOT_VOTING);
}

static void vote_led_apply_phase_base(const char *phase)
{
    if (!vote_led_enabled() || s_fault_latched) {
        return;
    }

    vote_led_cancel_base();
    (void)led_scene_cancel(LED_SCENE_ID_BALLOT_WIFI_WARN);

    if (s_wifi_offline) {
        (void)led_scene_run(LED_SCENE_ID_BALLOT_WIFI_WARN);
        return;
    }

    if (phase != NULL && strcmp(phase, "voting") == 0) {
        /* 绿常亮为底色；蓝慢闪 5 s 提示投票时段已开始（上电或到点进入 voting 各播一次） */
        (void)led_scene_run(LED_SCENE_ID_BALLOT_IDLE);
        (void)led_scene_run(LED_SCENE_ID_BALLOT_VOTING);
    } else {
        (void)led_scene_run(LED_SCENE_ID_BALLOT_IDLE);
    }
}

void vote_led_on_boot_ready(void)
{
    if (!vote_led_enabled()) {
        return;
    }

    (void)led_scene_cancel(LED_SCENE_ID_BOOTUP);
    vote_led_apply_phase_base(vote_status_current_phase(NULL));
}

void vote_led_sync_phase(const char *phase)
{
    if (phase == NULL) {
        return;
    }
    vote_led_apply_phase_base(phase);
}

void vote_led_on_ir_approach(void)
{
    if (!vote_led_enabled() || s_fault_latched) {
        return;
    }
    (void)led_scene_run(LED_SCENE_ID_BALLOT_APPROACH);
}

void vote_led_on_valid_vote(void)
{
    if (!vote_led_enabled() || s_fault_latched) {
        return;
    }
    (void)led_scene_run(LED_SCENE_ID_BALLOT_VALID);
}

void vote_led_on_spoiled_vote(void)
{
    if (!vote_led_enabled() || s_fault_latched) {
        return;
    }
    (void)led_scene_run(LED_SCENE_ID_BALLOT_SPOILED);
}

void vote_led_on_violation(void)
{
    if (!vote_led_enabled() || s_fault_latched) {
        return;
    }
    (void)led_scene_run(LED_SCENE_ID_BALLOT_VIOLATION);
}

void vote_led_on_violation_end(void)
{
    if (!vote_led_enabled() || s_fault_latched) {
        return;
    }
    (void)led_scene_cancel(LED_SCENE_ID_BALLOT_VIOLATION);
    vote_led_apply_phase_base(vote_status_current_phase(NULL));
}

void vote_led_on_fault(void)
{
    if (!vote_led_enabled()) {
        return;
    }
    s_fault_latched = true;
    for (uint8_t i = 0U; i < (uint8_t)LED_SCENE_ID_MAX_NUM; i++) {
        (void)led_scene_cancel((led_scene_id_e)i);
    }
    (void)led_scene_run_force(LED_SCENE_ID_BALLOT_FAULT);
}

void vote_led_on_wifi_sta_connected(void)
{
    if (!vote_led_enabled() || s_fault_latched) {
        return;
    }
    s_wifi_offline = false;
    (void)led_scene_cancel(LED_SCENE_ID_BALLOT_WIFI_WARN);
    vote_led_apply_phase_base(vote_status_current_phase(NULL));
}

void vote_led_on_wifi_sta_disconnected(void)
{
    if (!vote_led_enabled() || s_fault_latched) {
        return;
    }
    s_wifi_offline = true;
    vote_led_apply_phase_base(vote_status_current_phase(NULL));
}
