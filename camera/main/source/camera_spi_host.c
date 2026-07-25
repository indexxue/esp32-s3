/**
 * @file camera_spi_host.c
 * @brief SPI Master：1 MHz / 20 ms；对接 TM4C Slave（L0 线测或 HEARTBEAT）。
 */

#include "camera_spi_host.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "board.h"
#include "log.h"
#include "spi.h"
#include "spi_link.h"

#if defined(BOARD_PROFILE_CAMERA) && BOARD_SPI1_ENABLE

#define CAMERA_SPI_HOST_TASK_STACK_WORDS (3072U)
#define CAMERA_SPI_HOST_TASK_PRIORITY (3U)

/**
 * L0 物理通路测试（与 MCU CAMERA_SPI_WIRE_TEST 同步）：
 * 1 = 方案 A：TX 全 0x5A×32；0 = 正式 HEARTBEAT（L1）。
 */
#ifndef CAMERA_SPI_WIRE_TEST
#define CAMERA_SPI_WIRE_TEST (0)
#endif

#if !CAMERA_SPI_WIRE_TEST
#define CAMERA_SPI_HOST_LINK_OK_WINDOW_MS (1000U)
#define CAMERA_SPI_HOST_DOWN_STREAK (10U)
/** L1 安静期汇总（状态变化仍即时打）。 */
#define CAMERA_SPI_HOST_LOG_PERIOD_MS (5000U)
/** 1 = 周期附带 TX/RX 头 8 字节；联调确认后可改 0。 */
#ifndef CAMERA_SPI_HOST_LOG_VERBOSE
#define CAMERA_SPI_HOST_LOG_VERBOSE (1)
#endif
#else
/** 周期打印 TX/RX + 统计（避免每拍刷屏）。 */
#define CAMERA_SPI_HOST_LOG_PERIOD_MS (2000U)
/** MISO OK：至少连续这么多字节为 0xA5（理想 32）。 */
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
} camera_spi_stats_t;

static uint8_t s_tx_frame[SPI_LINK_FRAME_SIZE];
static uint8_t s_rx_frame[SPI_LINK_FRAME_SIZE];
static uint8_t s_tx_seq;
static camera_spi_stats_t s_stats;
static bool_t s_started = FALSE;

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
static uint16_t camera_spi_local_err_flags(void)
{
    uint16_t flags = 0U;

    if (s_stats.link == SPI_LINK_STATE_DOWN) {
        flags |= SPI_LINK_ERR_SLAVE_NOT_RESPONDING;
    }
    if (s_stats.crc_err >= 10U) {
        flags |= SPI_LINK_ERR_LINK_CRC_STORM;
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

static void camera_spi_update_link_state(uint32_t now_ms, bool_t frame_ok)
{
    const spi_link_state_t prev = s_stats.link;

    if (frame_ok != FALSE) {
        s_stats.bad_streak = 0U;
        s_stats.last_ok_ms = now_ms;
    } else if (s_stats.bad_streak < 0xFFFFFFFFu) {
        s_stats.bad_streak++;
    }

    if ((s_stats.last_ok_ms != 0U) &&
        ((now_ms - s_stats.last_ok_ms) <= CAMERA_SPI_HOST_LINK_OK_WINDOW_MS)) {
        /* 近期合法帧：连续失败中为 DEGRADED，否则 OK（lifetime crc 不永久钉死）。 */
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
    }
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

    LOG_INFO("spi1 init ok SCK=%d MOSI=%d MISO=%d CS=%d %uHz Mode1(CPOL0,CPHA1) poll=%ums L1_HB%s",
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
    uint16_t magic;
    bool_t ok = FALSE;
#endif

#if CAMERA_SPI_WIRE_TEST
    (void)memset(s_tx_frame, 0x5A, sizeof(s_tx_frame));
#else
    spi_link_build_heartbeat(s_tx_frame,
                             s_tx_seq,
                             now_ms,
                             SPI_LINK_ROLE_CAMERA_MASTER,
                             camera_spi_local_err_flags());
    s_tx_seq++;
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
    /* L0：不验 magic/CRC；协议 link 状态本阶段可忽略。 */
#else
    magic = (uint16_t)s_rx_frame[0] | ((uint16_t)s_rx_frame[1] << 8);
    if (magic != SPI_LINK_MAGIC) {
        s_stats.magic_err++;
        camera_spi_update_link_state(now_ms, FALSE);
        return;
    }

    if (spi_link_frame_check(s_rx_frame) == FALSE) {
        s_stats.crc_err++;
        camera_spi_update_link_state(now_ms, FALSE);
        return;
    }

    if (spi_link_frame_parse_hdr(s_rx_frame, &hdr) == FALSE) {
        s_stats.crc_err++;
        camera_spi_update_link_state(now_ms, FALSE);
        return;
    }

    s_stats.last_msg_id = hdr.msg_id;

    /* L1：期望 MCU HEARTBEAT，role=2；暂不处理 DETECT/SERVO/CTRL。 */
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

    s_stats.rx_ok++;
    ok = TRUE;
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
                "spi1: link=%s rx_ok=%lu magic_err=%lu crc_err=%lu xfer_err=%lu peer_role=%u",
                (s_stats.link == SPI_LINK_STATE_OK)         ? "OK"
                : (s_stats.link == SPI_LINK_STATE_DEGRADED) ? "DEGRADED"
                                                            : "DOWN",
                (unsigned long)s_stats.rx_ok,
                (unsigned long)s_stats.magic_err,
                (unsigned long)s_stats.crc_err,
                (unsigned long)s_stats.xfer_err,
                (unsigned)s_stats.peer_role);
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
