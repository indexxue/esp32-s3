/**
 * @file spi_link.h
 * @brief Camera↔MCU SPI 链路帧（32B）与 CRC-16/CCITT-FALSE；无硬件依赖。
 */

#ifndef COMMON_SPI_LINK_H
#define COMMON_SPI_LINK_H

#include "type.h"

#include <stddef.h>
#include <stdint.h>

#define SPI_LINK_FRAME_SIZE (32U)
#define SPI_LINK_PAYLOAD_MAX (22U)
#define SPI_LINK_HEADER_SIZE (8U)
#define SPI_LINK_CRC_SIZE (2U)

#define SPI_LINK_MAGIC (0xA55AU)
#define SPI_LINK_VER (0x01U)

#define SPI_LINK_MSG_HEARTBEAT (0x01U)
#define SPI_LINK_MSG_STATUS (0x02U)
#define SPI_LINK_MSG_DETECT_RESULT (0x10U)
#define SPI_LINK_MSG_SERVO_TELEMETRY (0x20U)
#define SPI_LINK_MSG_CTRL_CMD (0x30U)
#define SPI_LINK_MSG_CTRL_ACK (0x31U)

#define SPI_LINK_FLAG_ACK_REQ (1U << 0)
#define SPI_LINK_FLAG_SLAVE_HAS_CMD (1U << 1)
#define SPI_LINK_FLAG_NACK (1U << 2)
#define SPI_LINK_FLAG_BUSY (1U << 3)

#define SPI_LINK_ROLE_CAMERA_MASTER (1U)
#define SPI_LINK_ROLE_MCU_SLAVE (2U)

#define SPI_LINK_ERR_LINK_CRC_STORM (1U << 0)
#define SPI_LINK_ERR_SLAVE_NOT_RESPONDING (1U << 1)

typedef enum {
    SPI_LINK_STATE_DOWN = 0,
    SPI_LINK_STATE_DEGRADED = 1,
    SPI_LINK_STATE_OK = 2
} spi_link_state_t;

typedef struct {
    uint8_t ver;
    uint8_t seq;
    uint8_t msg_id;
    uint8_t flags;
    uint8_t len;
} spi_link_hdr_t;

typedef struct {
    uint32_t uptime_ms;
    uint8_t role;
    uint8_t proto_ver;
    uint16_t err_flags;
} spi_link_heartbeat_t;

uint16_t spi_link_crc16(const uint8_t *data, size_t len);

/** 必须：`123456789`→0x29B1，空 HEARTBEAT 前缀→0x6EB9。 */
bool_t spi_link_crc_selftest(void);

void spi_link_frame_clear(uint8_t frame[SPI_LINK_FRAME_SIZE]);
void spi_link_frame_set_crc(uint8_t frame[SPI_LINK_FRAME_SIZE]);
bool_t spi_link_frame_check(const uint8_t frame[SPI_LINK_FRAME_SIZE]);
bool_t spi_link_frame_parse_hdr(const uint8_t frame[SPI_LINK_FRAME_SIZE], spi_link_hdr_t *out);

void spi_link_build_heartbeat(uint8_t frame[SPI_LINK_FRAME_SIZE],
                              uint8_t seq,
                              uint32_t uptime_ms,
                              uint8_t role,
                              uint16_t err_flags);

bool_t spi_link_parse_heartbeat(const uint8_t frame[SPI_LINK_FRAME_SIZE],
                                spi_link_heartbeat_t *out);

#endif /* COMMON_SPI_LINK_H */
