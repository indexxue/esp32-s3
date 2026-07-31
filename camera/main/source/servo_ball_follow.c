/**
 * @file servo_ball_follow.c
 * @brief 平衡杠跟球：按球偏离中心打对面姿态；出画回中等待。
 *
 * 极性：默认「球偏左 → 打右」（平衡恢复）。反了只改 INVERT。
 * 幅度：离中心越远纠偏越大，越近越小（死区内回中）。
 */

#include "servo_ball_follow.h"

#include "camera_model.h"
#include "log.h"
#include "nvs.h"
#include "servo_ctrl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"

#define TASK_STACK   (3072U)
#define TASK_PRIO    (4U)
#define PERIOD_MS    (50U)
#define LOST_FRAMES  (12U)
#define FOUND_FRAMES (2U)
#define STEP_US_IN   (12U)
#define STEP_US_OUT  (80U)
#define WEIGHT_IN    (0.30f)
#define DEADZONE     (0.08f)

/**
 * 0=平衡极性：球偏左打右、偏右打左。
 * 1=取反。偏左却更左时改成 1。
 */
#ifndef SERVO_BALL_FOLLOW_INVERT
#define SERVO_BALL_FOLLOW_INVERT (0)
#endif

static bool_t            s_started;
static volatile bool_t   s_enabled = TRUE;
static nvs_servo_calib_t s_cal;
static servo_ch_t        s_ch = SERVO_CH_PAN;
static uint32_t          s_last_gen;
static uint16_t          s_lost;
static uint16_t          s_found;
static uint16_t          s_left_us;
static uint16_t          s_center_us;
static uint16_t          s_right_us;
static uint16_t          s_target_us;
static uint16_t          s_applied_us;
static bool_t            s_ball_out = FALSE;

static float us_per_deg(void)
{
    return (float)(BOARD_SERVO_PULSE_MAX_US - BOARD_SERVO_PULSE_MIN_US) /
           (float)BOARD_SERVO_ANGLE_MAX_DEG;
}

static uint16_t clamp_us(float us)
{
    if (us < (float)BOARD_SERVO_PULSE_MIN_US) {
        us = (float)BOARD_SERVO_PULSE_MIN_US;
    }
    if (us > (float)BOARD_SERVO_PULSE_MAX_US) {
        us = (float)BOARD_SERVO_PULSE_MAX_US;
    }
    return (uint16_t)(us + 0.5f);
}

static uint16_t pose_us(const nvs_servo_pose_t *pose)
{
    if (pose == NULL) {
        return (uint16_t)BOARD_SERVO_CENTER_PULSE_US;
    }
    return clamp_us((float)pose->pulse_us + (pose->offset_deg * us_per_deg()));
}

/**
 * 平衡律：err=nx-0.5；打向与球相反的一侧；|err| 越大倾角越大。
 */
static uint16_t target_from_nx(float nx)
{
    float err;
    float abs_err;
    float amount;
    float extreme_us;

    if (nx < 0.0f) {
        nx = 0.0f;
    }
    if (nx > 1.0f) {
        nx = 1.0f;
    }

    err     = nx - 0.5f;
    abs_err = (err >= 0.0f) ? err : -err;
    if (abs_err <= DEADZONE) {
        return s_center_us;
    }

    amount = (abs_err - DEADZONE) / (0.5f - DEADZONE);
    if (amount > 1.0f) {
        amount = 1.0f;
    }
    amount *= WEIGHT_IN;

#if SERVO_BALL_FOLLOW_INVERT
    /* 同侧（错误平衡极性时用） */
    extreme_us = (err < 0.0f) ? (float)s_left_us : (float)s_right_us;
#else
    /* 异侧：球在左(err<0) → 打右 */
    extreme_us = (err < 0.0f) ? (float)s_right_us : (float)s_left_us;
#endif

    return clamp_us((float)s_center_us + (amount * (extreme_us - (float)s_center_us)));
}

static bool_t pick_best_cx(const camera_model_result_t *r, uint16_t *cx_out)
{
    uint8_t  i;
    uint8_t  best = 0U;
    float    best_score = -1.0f;
    uint32_t cx;

    if ((r == NULL) || (r->count == 0U) || (r->frame_w == 0U) || (cx_out == NULL)) {
        return FALSE;
    }
    for (i = 0U; i < r->count; i++) {
        if (r->boxes[i].score > best_score) {
            best_score = r->boxes[i].score;
            best       = i;
        }
    }
    cx = (uint32_t)r->boxes[best].x + ((uint32_t)r->boxes[best].w / 2U);
    if (cx >= r->frame_w) {
        cx = (uint32_t)r->frame_w - 1U;
    }
    *cx_out = (uint16_t)cx;
    return TRUE;
}

static status_t step_toward(uint16_t target_us, uint16_t step_us)
{
    uint16_t cur = s_applied_us;
    int32_t  delta;
    uint16_t next;

    if (servo_get_pulse_us(s_ch, &cur) == STATUS_OK) {
        s_applied_us = cur;
    }
    delta = (int32_t)target_us - (int32_t)s_applied_us;
    if (delta == 0) {
        return STATUS_OK;
    }
    if (delta > (int32_t)step_us) {
        delta = (int32_t)step_us;
    } else if (delta < -(int32_t)step_us) {
        delta = -(int32_t)step_us;
    }
    next = (uint16_t)((int32_t)s_applied_us + delta);
    if (servo_set_pulse_us(s_ch, next) != STATUS_OK) {
        return STATUS_FAIL;
    }
    s_applied_us = next;
    return STATUS_OK;
}

static void enter_lost(void)
{
    s_ball_out  = TRUE;
    s_found     = 0U;
    s_target_us = s_center_us;
    (void)step_toward(s_target_us, STEP_US_OUT);
    LOG_INFO("ball follow LOST → center");
}

static void follow_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(PERIOD_MS);

    (void)arg;

    for (;;) {
        camera_model_result_t latest;
        uint32_t              gen = 0U;
        uint16_t              cx  = 0U;
        float                 nx;
        uint16_t              step_us;

        if ((s_enabled == FALSE) || (servo_is_ready() == FALSE) || (camera_model_is_ready() == FALSE) ||
            (camera_model_is_enabled() == FALSE)) {
            vTaskDelay(period);
            continue;
        }

        if (camera_model_get_latest_ex(&latest, &gen) == STATUS_OK) {
            if (gen != s_last_gen) {
                s_last_gen = gen;
                if (pick_best_cx(&latest, &cx) != FALSE) {
                    s_lost = 0U;
                    s_found++;
                    nx = ((float)cx + 0.5f) / (float)latest.frame_w;

                    if (s_ball_out != FALSE) {
                        if (s_found >= FOUND_FRAMES) {
                            s_ball_out  = FALSE;
                            s_target_us = s_center_us;
                            (void)step_toward(s_target_us, STEP_US_OUT);
                            LOG_INFO("ball follow FOUND nx=%.2f", (double)nx);
                        }
                    } else {
                        s_target_us = target_from_nx(nx);
                    }
                } else {
                    s_found = 0U;
                    s_lost++;
                    if ((s_lost >= LOST_FRAMES) && (s_ball_out == FALSE)) {
                        enter_lost();
                    }
                }
            }
        }

        if (s_ball_out != FALSE) {
            s_target_us = s_center_us;
            step_us     = STEP_US_OUT;
        } else {
            step_us = STEP_US_IN;
        }
        (void)step_toward(s_target_us, step_us);
        vTaskDelay(period);
    }
}

status_t servo_ball_follow_start(void)
{
    uint16_t cur = 0U;

    if (s_started != FALSE) {
        return STATUS_OK;
    }
    if (servo_is_ready() == FALSE) {
        LOG_WARN("ball follow: servo not ready");
        return STATUS_FAIL;
    }
    if (!nvs_servo_calib_get(&s_cal)) {
        LOG_WARN("ball follow: NVS servo_cal empty — skip");
        return STATUS_FAIL;
    }
    if ((s_cal.valid_mask & NVS_SERVO_CALIB_VALID_ALL) != NVS_SERVO_CALIB_VALID_ALL) {
        LOG_WARN("ball follow: need L+C+R in NVS (mask=0x%02X)", (unsigned)s_cal.valid_mask);
        return STATUS_FAIL;
    }

    s_ch        = (s_cal.channel == NVS_SERVO_CALIB_CH_TILT) ? SERVO_CH_TILT : SERVO_CH_PAN;
    s_center_us = pose_us(&s_cal.center);
    s_left_us   = pose_us(&s_cal.left);
    s_right_us  = pose_us(&s_cal.right);

    s_target_us = s_center_us;
    s_enabled   = TRUE;
    s_lost      = 0U;
    s_found     = 0U;
    s_last_gen  = 0U;
    s_ball_out  = FALSE;

    if (servo_set_pulse_us(s_ch, s_center_us) != STATUS_OK) {
        LOG_WARN("ball follow: park center failed");
        return STATUS_FAIL;
    }
    if (servo_get_pulse_us(s_ch, &cur) == STATUS_OK) {
        s_applied_us = cur;
    } else {
        s_applied_us = s_center_us;
    }

    if (xTaskCreate(follow_task, "ball_fol", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        LOG_ERROR("ball follow: task create failed");
        return STATUS_FAIL;
    }

    s_started = TRUE;
    LOG_INFO("ball follow start ch=%s invert=%d (0=balance) L/C/R=%u/%u/%u",
             (s_ch == SERVO_CH_TILT) ? "tilt" : "pan", SERVO_BALL_FOLLOW_INVERT, (unsigned)s_left_us,
             (unsigned)s_center_us, (unsigned)s_right_us);
    return STATUS_OK;
}

void servo_ball_follow_set_enabled(bool_t on)
{
    s_enabled = (on != FALSE) ? TRUE : FALSE;
}

bool_t servo_ball_follow_is_enabled(void)
{
    return s_enabled;
}
