/**
 * @file camera_spi_host.c
 * @brief SPI Master：Mode1 / 1 MHz / 20 ms / 32B；L1 HEARTBEAT + L2 上报 + L3 CTRL。
 */

#include "camera_spi_host.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "board.h"
#include "camera_model.h"
#include "log.h"
#include "servo_ctrl.h"
#include "spi.h"
#include "spi_link.h"

#if defined(BOARD_PROFILE_CAMERA) && BOARD_SPI1_ENABLE

#define CAMERA_SPI_HOST_TASK_STACK_WORDS (3072U)
#define CAMERA_SPI_HOST_TASK_PRIORITY (3U)

/**
 * L0 物理通路测试（与 MCU CAMERA_SPI_WIRE_TEST 同步）：
 * 1 = 方案 A：TX 全 0x5A×32；0 = 正式协议（L1+）。
 */
#ifndef CAMERA_SPI_WIRE_TEST
#define CAMERA_SPI_WIRE_TEST (0)
#endif

#if !CAMERA_SPI_WIRE_TEST
#define CAMERA_SPI_HOST_LINK_OK_WINDOW_MS (1000U)
#define CAMERA_SPI_HOST_DOWN_STREAK (10U)
/** L1 连续合法 HEARTBEAT(role=2) 达到该次数后才允许 L2 SERVO/DETECT。 */
#ifndef CAMERA_SPI_HOST_L1_STABLE_STREAK
#define CAMERA_SPI_HOST_L1_STABLE_STREAK (8U)
#endif
#define CAMERA_SPI_HOST_LOG_PERIOD_MS (5000U)
#ifndef CAMERA_SPI_HOST_LOG_VERBOSE
#define CAMERA_SPI_HOST_LOG_VERBOSE (1)
#endif
/** 舵机遥测周期（约 20 Hz）。 */
#ifndef CAMERA_SPI_SERVO_TEL_MS
#define CAMERA_SPI_SERVO_TEL_MS (50U)
#endif
/** 检测上报：有新结果代数则发；无新结果时最长间隔仍可重发最新（约 10 Hz）。 */
#ifndef CAMERA_SPI_DETECT_TX_MS
#define CAMERA_SPI_DETECT_TX_MS (100U)
#endif
#else
#define CAMERA_SPI_HOST_LOG_PERIOD_MS (2000U)
#define CAMERA_SPI_L0_A5_OK_RUN (8U)
#endif

typedef struct {
    uint32_t rx_ok;
    uint32_t crc_err;
    uint32_t magic_err;
    uint32_t xfer_err;
    uint32_t last_ok_ms;
    uint32_t bad_streak;
    spi_link_state_t link;
    uint8_t last_msg_id;
    uint32_t peer_uptime_ms;
    uint8_t peer_role;
    uint32_t detect_tx;
    uint32_t servo_tx;
    uint32_t ctrl_rx;
    uint32_t ctrl_ack_tx;
} camera_spi_stats_t;

static uint8_t s_tx_frame[SPI_LINK_FRAME_SIZE];
static uint8_t s_rx_frame[SPI_LINK_FRAME_SIZE];
static uint8_t s_tx_seq;
static camera_spi_stats_t s_stats;
static bool_t s_started = FALSE;

#if !CAMERA_SPI_WIRE_TEST
static bool_t s_slave_has_cmd = FALSE;
static bool_t s_detect_enabled = TRUE;
static bool_t s_ack_pending = FALSE;
static spi_link_ctrl_ack_t s_pending_ack;
static uint32_t s_last_detect_tx_ms;
static uint32_t s_last_servo_tx_ms;
static uint32_t s_last_detect_gen;
static bool_t s_had_detect_boxes = FALSE;
static bool_t s_force_servo_tel = FALSE;
static bool_t s_ctrl_rejected_recent = FALSE;
/** 连续合法 HB(role=2) 计数；达标后置位，允许 L2。 */
static uint32_t s_l1_ok_streak;
static bool_t s_l2_armed = FALSE;
#endif

#if CAMERA_SPI_WIRE_TEST
static size_t camera_spi_l0_a5_run(const uint8_t *p, size_t n)
{
    size_t best = 0U;
    size_t cur = 0U;
    size_t i;

    if (p == NULL) {
        return 0U;
    }

    for (i = 0U; i < n; i++) {
        if (p[i] == 0xA5U) {
            cur++;
            if (cur > best) {
                best = cur;
            }
        } else {
            cur = 0U;
        }
    }
    return best;
}

static void camera_spi_log_rx32(const uint8_t *p)
{
    if (p == NULL) {
        return;
    }
    LOG_INFO(
        "spi1 L0: RX32 "
        "%02X %02X %02X %02X %02X %02X %02X %02X "
        "%02X %02X %02X %02X %02X %02X %02X %02X "
        "%02X %02X %02X %02X %02X %02X %02X %02X "
        "%02X %02X %02X %02X %02X %02X %02X %02X",
        (unsigned)p[0],
        (unsigned)p[1],
        (unsigned)p[2],
        (unsigned)p[3],
        (unsigned)p[4],
        (unsigned)p[5],
        (unsigned)p[6],
        (unsigned)p[7],
        (unsigned)p[8],
        (unsigned)p[9],
        (unsigned)p[10],
        (unsigned)p[11],
        (unsigned)p[12],
        (unsigned)p[13],
        (unsigned)p[14],
        (unsigned)p[15],
        (unsigned)p[16],
        (unsigned)p[17],
        (unsigned)p[18],
        (unsigned)p[19],
        (unsigned)p[20],
        (unsigned)p[21],
        (unsigned)p[22],
        (unsigned)p[23],
        (unsigned)p[24],
        (unsigned)p[25],
        (unsigned)p[26],
        (unsigned)p[27],
        (unsigned)p[28],
        (unsigned)p[29],
        (unsigned)p[30],
        (unsigned)p[31]);
}
#else  /* !CAMERA_SPI_WIRE_TEST */

static int16_t camera_spi_deg_to_x100(float deg)
{
    const float scaled = deg * 100.0f;
    if (scaled > 32767.0f) {
        return 32767;
    }
    if (scaled < -32768.0f) {
        return -32768;
    }
    return (int16_t)scaled;
}

static uint16_t camera_spi_local_err_flags(void)
{
    uint16_t flags = 0U;

    if (s_stats.link == SPI_LINK_STATE_DOWN) {
        flags |= SPI_LINK_ERR_SLAVE_NOT_RESPONDING;
    }
    if (s_stats.crc_err >= 10U) {
        flags |= SPI_LINK_ERR_LINK_CRC_STORM;
    }
    if (s_ctrl_rejected_recent != FALSE) {
        flags |= SPI_LINK_ERR_CTRL_REJECTED_RECENT;
    }
    if (servo_is_ready() == FALSE) {
        flags |= SPI_LINK_ERR_SERVO_FAULT;
    }
    return flags;
}

#if CAMERA_SPI_HOST_LOG_VERBOSE
static void camera_spi_log_hex8(const char *tag, const uint8_t *p)
{
    if ((tag == NULL) || (p == NULL)) {
        return;
    }
    LOG_INFO("spi1: %s %02X %02X %02X %02X %02X %02X %02X %02X",
             tag,
             (unsigned)p[0],
             (unsigned)p[1],
             (unsigned)p[2],
             (unsigned)p[3],
             (unsigned)p[4],
             (unsigned)p[5],
             (unsigned)p[6],
             (unsigned)p[7]);
}
#endif

static void camera_spi_disarm_l2(const char *why)
{
    if (s_l2_armed != FALSE) {
        LOG_WARN("spi1: L2 disarm (%s)", (why != NULL) ? why : "");
    }
    s_l2_armed = FALSE;
    s_l1_ok_streak = 0U;
    s_force_servo_tel = FALSE;
    s_slave_has_cmd = FALSE;
}

static void camera_spi_note_l1_ok(void)
{
    if (s_l1_ok_streak < 0xFFFFFFFFu) {
        s_l1_ok_streak++;
    }
    if ((s_l2_armed == FALSE) && (s_l1_ok_streak >= CAMERA_SPI_HOST_L1_STABLE_STREAK)) {
        s_l2_armed = TRUE;
        LOG_INFO("spi1: L2 armed (L1 stable streak=%lu)", (unsigned long)s_l1_ok_streak);
    }
}

static bool_t camera_spi_l2_tx_allowed(void)
{
    return ((s_l2_armed != FALSE) && (s_stats.link == SPI_LINK_STATE_OK)) ? TRUE : FALSE;
}

static void camera_spi_update_link_state(uint32_t now_ms, bool_t frame_ok)
{
    const spi_link_state_t prev = s_stats.link;

    if (frame_ok != FALSE) {
        s_stats.bad_streak = 0U;
        s_stats.last_ok_ms = now_ms;
    } else if (s_stats.bad_streak < 0xFFFFFFFFu) {
        s_stats.bad_streak++;
        s_l1_ok_streak = 0U;
    }

    if ((s_stats.last_ok_ms != 0U) &&
        ((now_ms - s_stats.last_ok_ms) <= CAMERA_SPI_HOST_LINK_OK_WINDOW_MS)) {
        s_stats.link = (s_stats.bad_streak > 0U) ? SPI_LINK_STATE_DEGRADED : SPI_LINK_STATE_OK;
    } else if (s_stats.bad_streak >= CAMERA_SPI_HOST_DOWN_STREAK) {
        s_stats.link = SPI_LINK_STATE_DOWN;
    } else if (s_stats.rx_ok > 0U) {
        s_stats.link = SPI_LINK_STATE_DEGRADED;
    } else {
        s_stats.link = SPI_LINK_STATE_DOWN;
    }

    if ((s_stats.link != prev) && (s_stats.link == SPI_LINK_STATE_OK)) {
        LOG_INFO("spi1: link OK peer_role=%u", (unsigned)s_stats.peer_role);
    } else if ((s_stats.link != prev) && (s_stats.link == SPI_LINK_STATE_DEGRADED) &&
               (prev == SPI_LINK_STATE_OK)) {
        LOG_WARN("spi1: link DEGRADED");
    } else if ((s_stats.link != prev) && (s_stats.link == SPI_LINK_STATE_DOWN) &&
               (prev != SPI_LINK_STATE_DOWN)) {
        LOG_WARN("spi1: link DOWN");
        camera_spi_disarm_l2("link DOWN");
    }

    /* DEGRADED / 非 OK：暂停 L2，避免在抖动期继续灌 SERVO/DETECT。 */
    if ((s_stats.link != SPI_LINK_STATE_OK) && (s_l2_armed != FALSE)) {
        camera_spi_disarm_l2("link not OK");
    }
}

static uint8_t camera_spi_score_to_u8(float score)
{
    float scaled;

    if (score <= 0.0f) {
        return 0U;
    }
    if (score >= 1.0f) {
        return 255U;
    }
    scaled = score * 255.0f;
    if (scaled >= 255.0f) {
        return 255U;
    }
    return (uint8_t)scaled;
}

/**
 * 从 camera_model 取最新框，按 score 取最多 2 个；钢珠 class_id=0。
 * @return TRUE 且 *out_gen 有新代数（或超时重发）时调用方应发送。
 */
static bool_t camera_spi_fill_real_detect(spi_link_detect_result_t *det, uint32_t *out_gen)
{
    camera_model_result_t latest;
    uint32_t gen = 0U;
    uint8_t idx[CAMERA_MODEL_MAX_BOXES];
    uint8_t n;
    uint8_t i;
    uint8_t j;

    if ((det == NULL) || (out_gen == NULL)) {
        return FALSE;
    }
    if (camera_model_is_ready() == FALSE) {
        return FALSE;
    }
    if (camera_model_get_latest_ex(&latest, &gen) != STATUS_OK) {
        return FALSE;
    }

    (void)memset(det, 0, sizeof(*det));
    det->frame_w = latest.frame_w;
    det->frame_h = latest.frame_h;
    det->best_index = 0xFFU;

    n = latest.count;
    if (n > CAMERA_MODEL_MAX_BOXES) {
        n = CAMERA_MODEL_MAX_BOXES;
    }
    for (i = 0U; i < n; i++) {
        idx[i] = i;
    }
    /* 按 score 降序（插入排序，n≤8）。 */
    for (i = 1U; i < n; i++) {
        const uint8_t key = idx[i];
        j = i;
        while ((j > 0U) && (latest.boxes[idx[j - 1U]].score < latest.boxes[key].score)) {
            idx[j] = idx[j - 1U];
            j--;
        }
        idx[j] = key;
    }

    if (n > SPI_LINK_DETECT_BOX_MAX) {
        n = SPI_LINK_DETECT_BOX_MAX;
    }
    det->count = n;
    if (n > 0U) {
        det->best_index = 0U;
    }
    for (i = 0U; i < n; i++) {
        const camera_model_box_t *src = &latest.boxes[idx[i]];
        det->box[i].x = src->x;
        det->box[i].y = src->y;
        det->box[i].w = src->w;
        det->box[i].score_u8 = camera_spi_score_to_u8(src->score);
        det->box[i].class_id = 0U; /* 钢珠 */
    }

    *out_gen = gen;
    return TRUE;
}

static bool_t camera_spi_fill_servo_tel(spi_link_servo_telemetry_t *tel)
{
    float pan_deg = 0.0f;
    float tilt_deg = 0.0f;
    uint16_t pan_us = 0U;
    uint16_t tilt_us = 0U;
    servo_limits_t lim;

    if (tel == NULL) {
        return FALSE;
    }
    if (servo_is_ready() == FALSE) {
        return FALSE;
    }
    if (servo_get_angle(SERVO_CH_PAN, &pan_deg) != STATUS_OK) {
        return FALSE;
    }
    if (servo_get_angle(SERVO_CH_TILT, &tilt_deg) != STATUS_OK) {
        return FALSE;
    }
    if (servo_get_pulse_us(SERVO_CH_PAN, &pan_us) != STATUS_OK) {
        return FALSE;
    }
    if (servo_get_pulse_us(SERVO_CH_TILT, &tilt_us) != STATUS_OK) {
        return FALSE;
    }
    if (servo_get_limits(&lim) != STATUS_OK) {
        return FALSE;
    }

    (void)memset(tel, 0, sizeof(*tel));
    tel->pan_deg_x100 = camera_spi_deg_to_x100(pan_deg);
    tel->tilt_deg_x100 = camera_spi_deg_to_x100(tilt_deg);
    tel->pan_pulse_us = pan_us;
    tel->tilt_pulse_us = tilt_us;
    tel->pan_min_x100 = camera_spi_deg_to_x100(lim.pan_min_deg);
    tel->pan_max_x100 = camera_spi_deg_to_x100(lim.pan_max_deg);
    tel->tilt_min_x100 = camera_spi_deg_to_x100(lim.tilt_min_deg);
    tel->tilt_max_x100 = camera_spi_deg_to_x100(lim.tilt_max_deg);
    return TRUE;
}

static int16_t camera_spi_read_i16_le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void camera_spi_queue_ack(uint8_t sub_cmd, uint8_t req_id, uint8_t result, uint32_t detail)
{
    s_pending_ack.sub_cmd = sub_cmd;
    s_pending_ack.req_id = req_id;
    s_pending_ack.result = result;
    s_pending_ack.detail = detail;
    s_ack_pending = TRUE;
    if ((result != SPI_LINK_CTRL_RESULT_OK) && (result != SPI_LINK_CTRL_RESULT_UNSUPPORTED)) {
        s_ctrl_rejected_recent = TRUE;
    } else if (result == SPI_LINK_CTRL_RESULT_OK) {
        s_ctrl_rejected_recent = FALSE;
    }
}

static void camera_spi_log_servo_now(const char *why)
{
    float pan = 0.0f;
    float tilt = 0.0f;
    uint16_t pan_us = 0U;
    uint16_t tilt_us = 0U;

    if ((servo_get_angle(SERVO_CH_PAN, &pan) != STATUS_OK) ||
        (servo_get_angle(SERVO_CH_TILT, &tilt) != STATUS_OK) ||
        (servo_get_pulse_us(SERVO_CH_PAN, &pan_us) != STATUS_OK) ||
        (servo_get_pulse_us(SERVO_CH_TILT, &tilt_us) != STATUS_OK)) {
        return;
    }
    LOG_INFO("spi1: servo now %s pan=%d tilt=%d pulse=%u/%u",
             (why != NULL) ? why : "",
             (int)camera_spi_deg_to_x100(pan),
             (int)camera_spi_deg_to_x100(tilt),
             (unsigned)pan_us,
             (unsigned)tilt_us);
}

static void camera_spi_note_servo_moved(void)
{
    /* CTRL 改角后尽快插发 SERVO_TELEMETRY，避免 MCU 仍看到旧 18000。 */
    s_force_servo_tel = TRUE;
    s_last_servo_tx_ms = 0U;
    camera_spi_log_servo_now("after CTRL");
}

static void camera_spi_handle_ctrl_cmd(const spi_link_ctrl_cmd_t *cmd)
{
    uint8_t result = SPI_LINK_CTRL_RESULT_UNSUPPORTED;
    uint32_t detail = 0U;

    if (cmd == NULL) {
        return;
    }

    LOG_INFO("spi1: CTRL_CMD rx sub=0x%02X req=%u argc=%u",
             (unsigned)cmd->sub_cmd,
             (unsigned)cmd->req_id,
             (unsigned)cmd->argc);

    switch (cmd->sub_cmd) {
    case SPI_LINK_CTRL_SUB_DETECT_ENABLE:
        if (cmd->argc < 1U) {
            result = SPI_LINK_CTRL_RESULT_BAD_PARAM;
        } else if (cmd->args[0] > 1U) {
            result = SPI_LINK_CTRL_RESULT_BAD_PARAM;
        } else {
            const bool_t on = (cmd->args[0] != 0U) ? TRUE : FALSE;

            if (camera_model_set_enabled(on) != STATUS_OK) {
                result = SPI_LINK_CTRL_RESULT_FAILED;
            } else {
                s_detect_enabled = on;
                if (on == FALSE) {
                    s_last_detect_gen = 0U;
                }
                result = SPI_LINK_CTRL_RESULT_OK;
                detail = (uint32_t)s_detect_enabled;
            }
        }
        break;

    case SPI_LINK_CTRL_SUB_FOLLOW_ENABLE:
    case SPI_LINK_CTRL_SUB_SET_STREAM_MODE:
        result = SPI_LINK_CTRL_RESULT_UNSUPPORTED;
        break;

    case SPI_LINK_CTRL_SUB_SERVO_SET_ANGLE:
        if (cmd->argc < 3U) {
            result = SPI_LINK_CTRL_RESULT_BAD_PARAM;
        } else if (servo_is_ready() == FALSE) {
            result = SPI_LINK_CTRL_RESULT_FAILED;
        } else {
            const uint8_t ch = cmd->args[0];
            const int16_t deg_x100 = camera_spi_read_i16_le(&cmd->args[1]);
            const float deg = (float)deg_x100 / 100.0f;
            status_t st;

            if (ch == 0U) {
                st = servo_set_angle(SERVO_CH_PAN, deg);
            } else if (ch == 1U) {
                st = servo_set_angle(SERVO_CH_TILT, deg);
            } else {
                st = STATUS_FAIL;
                result = SPI_LINK_CTRL_RESULT_BAD_PARAM;
                break;
            }
            result = (st == STATUS_OK) ? SPI_LINK_CTRL_RESULT_OK : SPI_LINK_CTRL_RESULT_BAD_PARAM;
            if (result == SPI_LINK_CTRL_RESULT_OK) {
                camera_spi_note_servo_moved();
            }
        }
        break;

    case SPI_LINK_CTRL_SUB_SERVO_NUDGE:
        if (cmd->argc < 3U) {
            result = SPI_LINK_CTRL_RESULT_BAD_PARAM;
        } else if (servo_is_ready() == FALSE) {
            result = SPI_LINK_CTRL_RESULT_FAILED;
        } else {
            const uint8_t ch = cmd->args[0];
            const int16_t delta_x100 = camera_spi_read_i16_le(&cmd->args[1]);
            const float delta = (float)delta_x100 / 100.0f;
            status_t st;

            if (ch == 0U) {
                st = servo_nudge(SERVO_CH_PAN, delta);
            } else if (ch == 1U) {
                st = servo_nudge(SERVO_CH_TILT, delta);
            } else {
                result = SPI_LINK_CTRL_RESULT_BAD_PARAM;
                break;
            }
            result = (st == STATUS_OK) ? SPI_LINK_CTRL_RESULT_OK : SPI_LINK_CTRL_RESULT_BAD_PARAM;
            if (result == SPI_LINK_CTRL_RESULT_OK) {
                camera_spi_note_servo_moved();
            }
        }
        break;

    case SPI_LINK_CTRL_SUB_SERVO_CENTER:
        if (servo_is_ready() == FALSE) {
            result = SPI_LINK_CTRL_RESULT_FAILED;
        } else {
            result = (servo_center_all() == STATUS_OK) ? SPI_LINK_CTRL_RESULT_OK
                                                      : SPI_LINK_CTRL_RESULT_FAILED;
            if (result == SPI_LINK_CTRL_RESULT_OK) {
                camera_spi_note_servo_moved();
            }
        }
        break;

    case SPI_LINK_CTRL_SUB_SERVO_SET_LIMITS:
        if (cmd->argc < 8U) {
            result = SPI_LINK_CTRL_RESULT_BAD_PARAM;
        } else if (servo_is_ready() == FALSE) {
            result = SPI_LINK_CTRL_RESULT_FAILED;
        } else {
            const float pan_min = (float)camera_spi_read_i16_le(&cmd->args[0]) / 100.0f;
            const float pan_max = (float)camera_spi_read_i16_le(&cmd->args[2]) / 100.0f;
            const float tilt_min = (float)camera_spi_read_i16_le(&cmd->args[4]) / 100.0f;
            const float tilt_max = (float)camera_spi_read_i16_le(&cmd->args[6]) / 100.0f;
            result = (servo_set_limits(&pan_min, &pan_max, &tilt_min, &tilt_max) == STATUS_OK)
                         ? SPI_LINK_CTRL_RESULT_OK
                         : SPI_LINK_CTRL_RESULT_BAD_PARAM;
        }
        break;

    case SPI_LINK_CTRL_SUB_SERVO_RESET_LIMITS:
        if (servo_is_ready() == FALSE) {
            result = SPI_LINK_CTRL_RESULT_FAILED;
        } else {
            result = (servo_reset_limits() == STATUS_OK) ? SPI_LINK_CTRL_RESULT_OK
                                                        : SPI_LINK_CTRL_RESULT_FAILED;
        }
        break;

    default:
        result = SPI_LINK_CTRL_RESULT_UNSUPPORTED;
        break;
    }

    s_stats.ctrl_rx++;
    camera_spi_queue_ack(cmd->sub_cmd, cmd->req_id, result, detail);
}

static void camera_spi_build_tx(uint32_t now_ms)
{
    spi_link_detect_result_t det;
    spi_link_servo_telemetry_t tel;

    /*
     * CTRL_ACK 先于 HAS_CMD yield：MCU 若在 CTRL_CMD 帧仍带 HAS_CMD，
     * 也必须先把 ACK 送出完成对账。
     */
    if (s_ack_pending != FALSE) {
        const spi_link_ctrl_ack_t ack = s_pending_ack;

        spi_link_build_ctrl_ack(s_tx_frame, s_tx_seq, &ack);
        s_tx_seq++;
        s_ack_pending = FALSE;
        s_stats.ctrl_ack_tx++;
        LOG_INFO("spi1: CTRL_ACK tx sub=0x%02X req=%u result=%u",
                 (unsigned)ack.sub_cmd,
                 (unsigned)ack.req_id,
                 (unsigned)ack.result);
        return;
    }

    /*
     * L1 未稳定 / 链路非 OK：只发 HEARTBEAT 探测，禁止 L2 SERVO/DETECT。
     * 否则 DOWN 时仍灌 SERVO，易导致 TM4C SSI 预装卡住，只能重启 MCU 恢复。
     */
    if (camera_spi_l2_tx_allowed() == FALSE) {
        spi_link_build_heartbeat(s_tx_frame,
                                 s_tx_seq,
                                 now_ms,
                                 SPI_LINK_ROLE_CAMERA_MASTER,
                                 camera_spi_local_err_flags());
        s_tx_seq++;
        return;
    }

    /* §4.5：上拍 HAS_CMD → 本拍只发 HEARTBEAT，便于 MCU 吐 CTRL_CMD。 */
    if (s_slave_has_cmd != FALSE) {
        spi_link_build_heartbeat(s_tx_frame,
                                 s_tx_seq,
                                 now_ms,
                                 SPI_LINK_ROLE_CAMERA_MASTER,
                                 camera_spi_local_err_flags());
        s_tx_seq++;
        return;
    }

    /* CTRL 刚改过角度：优先推一帧真实 SERVO，再走常规 DETECT/周期遥测。 */
    if (s_force_servo_tel != FALSE) {
        if (camera_spi_fill_servo_tel(&tel) != FALSE) {
            spi_link_build_servo_telemetry(s_tx_frame, s_tx_seq, &tel);
            s_tx_seq++;
            s_last_servo_tx_ms = now_ms;
            s_force_servo_tel = FALSE;
            s_stats.servo_tx++;
            LOG_INFO("spi1: SERVO tx pan=%d tilt=%d (forced)",
                     (int)tel.pan_deg_x100,
                     (int)tel.tilt_deg_x100);
            return;
        }
        s_force_servo_tel = FALSE;
    }

    if (s_detect_enabled != FALSE) {
        uint32_t gen = 0U;
        const bool_t have = camera_spi_fill_real_detect(&det, &gen);
        const bool_t is_new = (have != FALSE) && (gen != s_last_detect_gen);
        const bool_t due =
            (have != FALSE) && ((now_ms - s_last_detect_tx_ms) >= CAMERA_SPI_DETECT_TX_MS);
        bool_t should_tx = FALSE;

        if ((have != FALSE) && ((is_new != FALSE) || (due != FALSE))) {
            if (det.count > 0U) {
                should_tx = TRUE;
                s_had_detect_boxes = TRUE;
            } else if (s_had_detect_boxes != FALSE) {
                /* 由有框→无框时发一帧 n=0，之后不再刷空 DETECT。 */
                should_tx = TRUE;
                s_had_detect_boxes = FALSE;
            } else {
                s_last_detect_gen = gen;
                s_last_detect_tx_ms = now_ms;
            }
        }

        if (should_tx != FALSE) {
            spi_link_build_detect_result(s_tx_frame, s_tx_seq, &det);
            s_tx_seq++;
            s_last_detect_tx_ms = now_ms;
            s_last_detect_gen = gen;
            s_stats.detect_tx++;
            if ((s_stats.detect_tx == 1U) || ((s_stats.detect_tx % 20U) == 0U) ||
                (det.count == 0U)) {
                LOG_INFO("spi1: DETECT tx n=%u x=%u y=%u w=%u sc=%u",
                         (unsigned)det.count,
                         (unsigned)((det.count > 0U) ? det.box[0].x : 0U),
                         (unsigned)((det.count > 0U) ? det.box[0].y : 0U),
                         (unsigned)((det.count > 0U) ? det.box[0].w : 0U),
                         (unsigned)((det.count > 0U) ? det.box[0].score_u8 : 0U));
            }
            return;
        }
    }

    if ((now_ms - s_last_servo_tx_ms) >= CAMERA_SPI_SERVO_TEL_MS) {
        if (camera_spi_fill_servo_tel(&tel) != FALSE) {
            spi_link_build_servo_telemetry(s_tx_frame, s_tx_seq, &tel);
            s_tx_seq++;
            s_last_servo_tx_ms = now_ms;
            s_stats.servo_tx++;
            if ((s_stats.servo_tx == 1U) || ((s_stats.servo_tx % 40U) == 0U)) {
                LOG_INFO("spi1: SERVO tx pan=%d tilt=%d",
                         (int)tel.pan_deg_x100,
                         (int)tel.tilt_deg_x100);
            }
            return;
        }
    }

    spi_link_build_heartbeat(s_tx_frame,
                             s_tx_seq,
                             now_ms,
                             SPI_LINK_ROLE_CAMERA_MASTER,
                             camera_spi_local_err_flags());
    s_tx_seq++;
}
#endif /* CAMERA_SPI_WIRE_TEST */

static status_t camera_spi_bus_init(void)
{
    SpiDriverConfig_t busCfg = {0};
    SpiDeviceConfig_t devCfg = {0};

    busCfg.host = BOARD_SPI1_HOST;
    busCfg.sclkPin = (s32_t)BOARD_SPI1_PIN_SCK;
    busCfg.mosiPin = (s32_t)BOARD_SPI1_PIN_MOSI;
    busCfg.misoPin = (s32_t)BOARD_SPI1_PIN_MISO;
    busCfg.quadWpPin = -1;
    busCfg.quadHdPin = -1;
    busCfg.maxTransferSize = (s32_t)SPI_LINK_FRAME_SIZE;
    busCfg.dmaChannel = DMA_SPI_BUS_DISABLED_E;
    busCfg.intrFlags = 0;
    busCfg.maxDeviceCount = 1U;

    if (SpiDriverInit(&busCfg) != TRUE) {
        LOG_ERROR("spi1: SpiDriverInit failed, esp err %d", (int)SpiGetLastError());
        return STATUS_FAIL;
    }

    devCfg.host = BOARD_SPI1_HOST;
    devCfg.chipSelectPin = (s32_t)BOARD_SPI1_PIN_CS;
    devCfg.clockSpeedHz = BOARD_SPI1_CLOCK_HZ;
    /* Mode1：TM4C SSI Slave 整帧 CS 低时需 CPHA=1，否则仅首字节有效。 */
    devCfg.mode = SPI_CLOCK_MODE_1_E;
    devCfg.flags = 0U;
    devCfg.queueSize = 1U;
    devCfg.csEnaPretrans = 2U;
    devCfg.csEnaPosttrans = 2U;

    if (SpiRegisterDevice(&devCfg) != TRUE) {
        LOG_ERROR("spi1: SpiRegisterDevice failed, esp err %d", (int)SpiGetLastError());
        return STATUS_FAIL;
    }

    LOG_INFO("spi1 init ok SCK=%d MOSI=%d MISO=%d CS=%d %uHz Mode1(CPOL0,CPHA1) poll=%ums L2L3%s",
             BOARD_SPI1_PIN_SCK,
             BOARD_SPI1_PIN_MOSI,
             BOARD_SPI1_PIN_MISO,
             BOARD_SPI1_PIN_CS,
             (unsigned)BOARD_SPI1_CLOCK_HZ,
             (unsigned)BOARD_SPI1_POLL_MS,
             CAMERA_SPI_WIRE_TEST ? " L0_WIRE_TEST" : "");
    return STATUS_OK;
}

static void camera_spi_poll_once(void)
{
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
#if !CAMERA_SPI_WIRE_TEST
    spi_link_hdr_t hdr;
    spi_link_heartbeat_t hb;
    spi_link_ctrl_cmd_t cmd;
    uint16_t magic;
    bool_t ok = FALSE;
#endif

#if CAMERA_SPI_WIRE_TEST
    (void)memset(s_tx_frame, 0x5A, sizeof(s_tx_frame));
#else
    camera_spi_build_tx(now_ms);
#endif

    (void)memset(s_rx_frame, 0xFF, sizeof(s_rx_frame));

    if (SpiTransmitReceive((s32_t)BOARD_SPI1_PIN_CS, s_tx_frame, s_rx_frame, SPI_LINK_FRAME_SIZE) !=
        TRUE) {
        s_stats.xfer_err++;
#if !CAMERA_SPI_WIRE_TEST
        camera_spi_update_link_state(now_ms, FALSE);
#endif
        return;
    }

#if CAMERA_SPI_WIRE_TEST
    (void)now_ms;
#else
    magic = (uint16_t)s_rx_frame[0] | ((uint16_t)s_rx_frame[1] << 8);
    if (magic != SPI_LINK_MAGIC) {
        s_stats.magic_err++;
        s_slave_has_cmd = FALSE;
        camera_spi_update_link_state(now_ms, FALSE);
        return;
    }

    if (spi_link_frame_check(s_rx_frame) == FALSE) {
        s_stats.crc_err++;
        s_slave_has_cmd = FALSE;
        camera_spi_update_link_state(now_ms, FALSE);
        return;
    }

    if (spi_link_frame_parse_hdr(s_rx_frame, &hdr) == FALSE) {
        s_stats.crc_err++;
        s_slave_has_cmd = FALSE;
        camera_spi_update_link_state(now_ms, FALSE);
        return;
    }

    s_stats.last_msg_id = hdr.msg_id;
    {
        const bool_t has_cmd =
            ((hdr.flags & SPI_LINK_FLAG_SLAVE_HAS_CMD) != 0U) ? TRUE : FALSE;
        if ((has_cmd != FALSE) && (s_slave_has_cmd == FALSE)) {
            LOG_INFO("spi1: HAS_CMD seen → TX HEARTBEAT (yield)");
        }
        s_slave_has_cmd = has_cmd;
    }

    if (hdr.msg_id == SPI_LINK_MSG_HEARTBEAT) {
        if (spi_link_parse_heartbeat(s_rx_frame, &hb) == FALSE) {
            camera_spi_update_link_state(now_ms, FALSE);
            return;
        }
        s_stats.peer_uptime_ms = hb.uptime_ms;
        s_stats.peer_role = hb.role;
        if (hb.role != SPI_LINK_ROLE_MCU_SLAVE) {
            camera_spi_update_link_state(now_ms, FALSE);
            return;
        }
        ok = TRUE;
        camera_spi_note_l1_ok();
    } else if (hdr.msg_id == SPI_LINK_MSG_CTRL_CMD) {
        if (spi_link_parse_ctrl_cmd(s_rx_frame, &cmd) != FALSE) {
            camera_spi_handle_ctrl_cmd(&cmd);
        } else {
            LOG_WARN("spi1: CTRL_CMD parse fail len=%u", (unsigned)hdr.len);
        }
        /* CTRL 也证明链路活着，但不计入 L1 稳定 streak（协议：先 HB 稳定再 L2）。 */
        ok = TRUE;
        s_l1_ok_streak = 0U;
    } else if (hdr.msg_id == SPI_LINK_MSG_STATUS) {
        ok = TRUE;
        s_l1_ok_streak = 0U;
    } else {
        /* 其它合法帧：计链路 OK，不执行；打断 L1 streak。 */
        ok = TRUE;
        s_l1_ok_streak = 0U;
    }

    if (ok != FALSE) {
        s_stats.rx_ok++;
    }
    camera_spi_update_link_state(now_ms, ok);
#endif
}

static void camera_spi_host_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(BOARD_SPI1_POLL_MS);
    uint32_t last_log_ms = 0U;

    (void)arg;

    for (;;) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);

        camera_spi_poll_once();

        if ((now_ms - last_log_ms) >= CAMERA_SPI_HOST_LOG_PERIOD_MS) {
            last_log_ms = now_ms;
#if CAMERA_SPI_WIRE_TEST
            {
                const size_t a5_run = camera_spi_l0_a5_run(s_rx_frame, SPI_LINK_FRAME_SIZE);
                LOG_INFO("spi1 L0: TX=5Ax32");
                camera_spi_log_rx32(s_rx_frame);
                LOG_INFO("spi1 L0: MISO=%s a5_run=%u xfer_err=%lu",
                         (a5_run >= CAMERA_SPI_L0_A5_OK_RUN) ? "OK" : "FAIL",
                         (unsigned)a5_run,
                         (unsigned long)s_stats.xfer_err);
            }
#else
#if CAMERA_SPI_HOST_LOG_VERBOSE
            camera_spi_log_hex8("TX", s_tx_frame);
            camera_spi_log_hex8("RX", s_rx_frame);
            {
                unsigned i;
                bool_t all0 = TRUE;
                bool_t allff = TRUE;
                for (i = 0U; i < 8U; i++) {
                    if (s_rx_frame[i] != 0x00U) {
                        all0 = FALSE;
                    }
                    if (s_rx_frame[i] != 0xFFU) {
                        allff = FALSE;
                    }
                }
                if (all0 != FALSE) {
                    LOG_WARN("spi1: MISO stuck LOW (RX all 00) — check MCU Mode1 + MISO pin/wire");
                } else if (allff != FALSE) {
                    LOG_WARN("spi1: MISO float/idle HIGH (RX all FF) — check CS/wire/MCU running");
                }
            }
#endif
            LOG_INFO(
                "spi1: link=%s l2=%s rx_ok=%lu magic_err=%lu crc_err=%lu xfer_err=%lu "
                "peer_role=%u det_tx=%lu servo_tx=%lu ctrl_rx=%lu ack_tx=%lu",
                (s_stats.link == SPI_LINK_STATE_OK)         ? "OK"
                : (s_stats.link == SPI_LINK_STATE_DEGRADED) ? "DEGRADED"
                                                            : "DOWN",
                (s_l2_armed != FALSE) ? "ON" : "OFF",
                (unsigned long)s_stats.rx_ok,
                (unsigned long)s_stats.magic_err,
                (unsigned long)s_stats.crc_err,
                (unsigned long)s_stats.xfer_err,
                (unsigned)s_stats.peer_role,
                (unsigned long)s_stats.detect_tx,
                (unsigned long)s_stats.servo_tx,
                (unsigned long)s_stats.ctrl_rx,
                (unsigned long)s_stats.ctrl_ack_tx);
#endif
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

status_t camera_spi_host_start(void)
{
    if (s_started != FALSE) {
        return STATUS_OK;
    }

#if !CAMERA_SPI_WIRE_TEST
    if (spi_link_crc_selftest() == FALSE) {
        LOG_ERROR("spi1: CRC selftest failed");
        return STATUS_FAIL;
    }
#endif

    if (camera_spi_bus_init() != STATUS_OK) {
        return STATUS_FAIL;
    }

    (void)memset(&s_stats, 0, sizeof(s_stats));
    s_stats.link = SPI_LINK_STATE_DOWN;
    s_tx_seq = 0U;
#if !CAMERA_SPI_WIRE_TEST
    s_slave_has_cmd = FALSE;
    s_detect_enabled = TRUE;
    s_ack_pending = FALSE;
    s_last_detect_tx_ms = 0U;
    s_last_servo_tx_ms = 0U;
    s_last_detect_gen = 0U;
    s_had_detect_boxes = FALSE;
    s_force_servo_tel = FALSE;
    s_ctrl_rejected_recent = FALSE;
    s_l1_ok_streak = 0U;
    s_l2_armed = FALSE;
    (void)memset(&s_pending_ack, 0, sizeof(s_pending_ack));
#endif

    if (xTaskCreate(camera_spi_host_task, "spi1_host", CAMERA_SPI_HOST_TASK_STACK_WORDS, NULL,
                    CAMERA_SPI_HOST_TASK_PRIORITY, NULL) != pdPASS) {
        LOG_ERROR("spi1: create task failed");
        return STATUS_FAIL;
    }

    s_started = TRUE;
    return STATUS_OK;
}

#else /* !BOARD_PROFILE_CAMERA || !BOARD_SPI1_ENABLE */

status_t camera_spi_host_start(void)
{
    return STATUS_OK;
}

#endif
