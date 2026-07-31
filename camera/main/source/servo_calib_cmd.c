/**
 * @file servo_calib_cmd.c
 * @brief USB 串口舵机调试 + NVS 校准值转储/自检。
 *
 * 用法（idf monitor / USB 串口，行尾回车）：
 *   help
 *   servo
 *   servo cal              # 读 NVS L/C/R 并判断顺序是否合理
 *   servo pan
 *   servo tilt 180
 *   servo pan pulse 1500
 *   servo tilt nudge 5
 *   servo center
 */

#include "servo_calib_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "cmd.h"
#include "log.h"
#include "nvs.h"
#include "servo_ctrl.h"

static servo_ch_t servo_calib_cmd_parse_ch(const char *name)
{
    if ((name != NULL) && (strcmp(name, "tilt") == 0)) {
        return SERVO_CH_TILT;
    }
    return SERVO_CH_PAN;
}

static const char *servo_calib_cmd_ch_name(servo_ch_t ch)
{
    return (ch == SERVO_CH_TILT) ? "tilt" : "pan";
}

static int servo_calib_cmd_gpio(servo_ch_t ch)
{
    return (ch == SERVO_CH_TILT) ? BOARD_SERVO_TILT_PIN : BOARD_SERVO_PAN_PIN;
}

static float servo_calib_cmd_us_per_deg(void)
{
    return (float)(BOARD_SERVO_PULSE_MAX_US - BOARD_SERVO_PULSE_MIN_US) /
           (float)BOARD_SERVO_ANGLE_MAX_DEG;
}

/** 跟球实际用的脉宽：pulse_us + offset_deg 换算。 */
static uint16_t servo_calib_cmd_eff_us(const nvs_servo_pose_t *pose)
{
    float us;

    if (pose == NULL) {
        return (uint16_t)BOARD_SERVO_CENTER_PULSE_US;
    }
    us = (float)pose->pulse_us + (pose->offset_deg * servo_calib_cmd_us_per_deg());
    if (us < (float)BOARD_SERVO_PULSE_MIN_US) {
        us = (float)BOARD_SERVO_PULSE_MIN_US;
    }
    if (us > (float)BOARD_SERVO_PULSE_MAX_US) {
        us = (float)BOARD_SERVO_PULSE_MAX_US;
    }
    return (uint16_t)(us + 0.5f);
}

static void servo_calib_cmd_dump_nvs(void)
{
    nvs_servo_calib_t cal;
    uint16_t          l_us;
    uint16_t          c_us;
    uint16_t          r_us;
    int               dl;
    int               dr;
    const char       *order;
    const char       *hint;

    if (!nvs_servo_calib_get(&cal)) {
        cmd_reply_ng();
        LOG_WARN("servo cal: NVS empty — 请烧校准固件重新标定左/中/右");
        return;
    }

    l_us = servo_calib_cmd_eff_us(&cal.left);
    c_us = servo_calib_cmd_eff_us(&cal.center);
    r_us = servo_calib_cmd_eff_us(&cal.right);
    dl   = (int)l_us - (int)c_us;
    dr   = (int)r_us - (int)c_us;

    if ((cal.valid_mask & NVS_SERVO_CALIB_VALID_ALL) != NVS_SERVO_CALIB_VALID_ALL) {
        order = "incomplete";
        hint  = "valid_mask 缺姿态，请补齐 L+C+R";
    } else if ((l_us < c_us) && (c_us < r_us)) {
        order = "L<C<R";
        hint  = "脉宽递增正常。识别固件 INVERT=0：球偏左应打右(R)。若偏左更左 → INVERT=1";
    } else if ((l_us > c_us) && (c_us > r_us)) {
        order = "L>C>R";
        hint  = "脉宽递减（与常见相反）。可重标使 L<C<R，或识别里 INVERT=1 试极性";
    } else {
        order = "non_monotonic";
        hint  = "L/C/R 非单调，极易跟球乱抖 — 必须重标：物理左端=Left，中=Center，右端=Right";
    }

    LOG_INFO("======== servo_cal NVS dump ========");
    LOG_INFO("channel=%s mask=0x%02X order=%s",
             (cal.channel == NVS_SERVO_CALIB_CH_TILT) ? "tilt" : "pan", (unsigned)cal.valid_mask, order);
    LOG_INFO("LEFT   angle=%.1f offset=%+.2f pulse=%u  eff_us=%u  (C%+d)",
             (double)cal.left.angle_deg, (double)cal.left.offset_deg, (unsigned)cal.left.pulse_us,
             (unsigned)l_us, dl);
    LOG_INFO("CENTER angle=%.1f offset=%+.2f pulse=%u  eff_us=%u",
             (double)cal.center.angle_deg, (double)cal.center.offset_deg, (unsigned)cal.center.pulse_us,
             (unsigned)c_us);
    LOG_INFO("RIGHT  angle=%.1f offset=%+.2f pulse=%u  eff_us=%u  (C%+d)",
             (double)cal.right.angle_deg, (double)cal.right.offset_deg, (unsigned)cal.right.pulse_us,
             (unsigned)r_us, dr);
    LOG_INFO("span L↔R = %d us | hint: %s", (int)r_us - (int)l_us, hint);
    LOG_INFO("====================================");

    cmd_reply_ok("servo", order);
}

static void servo_calib_cmd_reply_status(servo_ch_t ch)
{
    float    deg = 0.0f;
    uint16_t us  = 0U;
    char     val[96];

    if (servo_is_ready() == FALSE) {
        cmd_reply_ng();
        LOG_WARN("servo cmd: not ready");
        return;
    }
    if ((servo_get_angle(ch, &deg) != STATUS_OK) || (servo_get_pulse_us(ch, &us) != STATUS_OK)) {
        cmd_reply_ng();
        return;
    }
    (void)snprintf(val, sizeof(val), "%s gpio=%d deg=%.1f us=%u ready=1", servo_calib_cmd_ch_name(ch),
                   servo_calib_cmd_gpio(ch), (double)deg, (unsigned)us);
    cmd_reply_ok("servo", val);
    LOG_INFO("servo cmd status %s", val);
}

static void cmd_servo(int argc, const char *argv[])
{
    servo_ch_t ch;
    float      v;
    char      *end = NULL;
    status_t   st;
    char       val[64];

    if (servo_is_ready() == FALSE) {
        LOG_WARN("servo cmd: not ready");
        cmd_reply_ng();
        return;
    }

    if (argc <= 1) {
        servo_calib_cmd_reply_status(SERVO_CH_PAN);
        servo_calib_cmd_reply_status(SERVO_CH_TILT);
        return;
    }

    if (strcmp(argv[1], "center") == 0) {
        st = servo_center_all();
        if (st != STATUS_OK) {
            cmd_reply_ng();
            return;
        }
        cmd_reply_ok("servo", "center");
        LOG_INFO("servo cmd: center_all ok");
        return;
    }

    if (strcmp(argv[1], "cal") == 0) {
        servo_calib_cmd_dump_nvs();
        return;
    }

    if ((strcmp(argv[1], "pan") != 0) && (strcmp(argv[1], "tilt") != 0)) {
        cmd_reply_ng();
        return;
    }

    ch = servo_calib_cmd_parse_ch(argv[1]);

    if (argc == 2) {
        servo_calib_cmd_reply_status(ch);
        return;
    }

    if (strcmp(argv[2], "pulse") == 0) {
        unsigned long pulse;

        if (argc < 4) {
            cmd_reply_ng();
            return;
        }
        pulse = strtoul(argv[3], &end, 10);
        if ((end == argv[3]) || (pulse > 65535UL)) {
            cmd_reply_ng();
            return;
        }
        st = servo_set_pulse_us(ch, (uint16_t)pulse);
        if (st != STATUS_OK) {
            cmd_reply_ng();
            return;
        }
        (void)snprintf(val, sizeof(val), "%s pulse=%lu", servo_calib_cmd_ch_name(ch), pulse);
        cmd_reply_ok("servo", val);
        LOG_INFO("servo cmd: %s", val);
        return;
    }

    if (strcmp(argv[2], "nudge") == 0) {
        if (argc < 4) {
            cmd_reply_ng();
            return;
        }
        v = strtof(argv[3], &end);
        if (end == argv[3]) {
            cmd_reply_ng();
            return;
        }
        st = servo_nudge(ch, v);
        if (st != STATUS_OK) {
            cmd_reply_ng();
            return;
        }
        (void)snprintf(val, sizeof(val), "%s nudge=%+.1f", servo_calib_cmd_ch_name(ch), (double)v);
        cmd_reply_ok("servo", val);
        LOG_INFO("servo cmd: %s", val);
        servo_calib_cmd_reply_status(ch);
        return;
    }

    /* servo pan 180  /  servo tilt 90.5 */
    v = strtof(argv[2], &end);
    if (end == argv[2]) {
        cmd_reply_ng();
        return;
    }
    st = servo_set_angle(ch, v);
    if (st != STATUS_OK) {
        cmd_reply_ng();
        return;
    }
    (void)snprintf(val, sizeof(val), "%s angle=%.1f", servo_calib_cmd_ch_name(ch), (double)v);
    cmd_reply_ok("servo", val);
    LOG_INFO("servo cmd: %s", val);
    servo_calib_cmd_reply_status(ch);
}

void servo_calib_cmd_register(void)
{
    (void)cmd_register("servo", cmd_servo, "servo [cal|pan|tilt|center] ...");
}
