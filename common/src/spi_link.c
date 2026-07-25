#include "spi_link.h"

#include <stddef.h>
#include <string.h>

uint16_t spi_link_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;
    size_t i;
    int b;

    if (data == NULL) {
        return 0U;
    }

    for (i = 0U; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (b = 0; b < 8; b++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

bool_t spi_link_crc_selftest(void)
{
    static const uint8_t k_ascii[] = {
        '1', '2', '3', '4', '5', '6', '7', '8', '9'
    };
    /* magic LE + ver/seq/msg/flags/len/rsv + 22 zero payload */
    static const uint8_t k_hb_prefix[30] = {
        0x5AU, 0xA5U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
    };

    if (spi_link_crc16(k_ascii, sizeof(k_ascii)) != 0x29B1U) {
        return FALSE;
    }
    if (spi_link_crc16(k_hb_prefix, sizeof(k_hb_prefix)) != 0x6EB9U) {
        return FALSE;
    }
    return TRUE;
}

void spi_link_frame_clear(uint8_t frame[SPI_LINK_FRAME_SIZE])
{
    if (frame == NULL) {
        return;
    }
    (void)memset(frame, 0, SPI_LINK_FRAME_SIZE);
}

void spi_link_frame_set_crc(uint8_t frame[SPI_LINK_FRAME_SIZE])
{
    uint16_t crc;

    if (frame == NULL) {
        return;
    }
    crc = spi_link_crc16(frame, SPI_LINK_FRAME_SIZE - SPI_LINK_CRC_SIZE);
    frame[30] = (uint8_t)(crc & 0xFFU);
    frame[31] = (uint8_t)((crc >> 8) & 0xFFU);
}

bool_t spi_link_frame_check(const uint8_t frame[SPI_LINK_FRAME_SIZE])
{
    uint16_t expect;
    uint16_t got;
    uint16_t magic;
    uint8_t len;

    if (frame == NULL) {
        return FALSE;
    }

    magic = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    if (magic != SPI_LINK_MAGIC) {
        return FALSE;
    }
    if (frame[2] != SPI_LINK_VER) {
        return FALSE;
    }
    len = frame[6];
    if (len > SPI_LINK_PAYLOAD_MAX) {
        return FALSE;
    }

    expect = spi_link_crc16(frame, SPI_LINK_FRAME_SIZE - SPI_LINK_CRC_SIZE);
    got = (uint16_t)frame[30] | ((uint16_t)frame[31] << 8);
    return (expect == got) ? TRUE : FALSE;
}

bool_t spi_link_frame_parse_hdr(const uint8_t frame[SPI_LINK_FRAME_SIZE], spi_link_hdr_t *out)
{
    if ((frame == NULL) || (out == NULL)) {
        return FALSE;
    }
    if (spi_link_frame_check(frame) == FALSE) {
        return FALSE;
    }
    out->ver = frame[2];
    out->seq = frame[3];
    out->msg_id = frame[4];
    out->flags = frame[5];
    out->len = frame[6];
    return TRUE;
}

void spi_link_build_heartbeat(uint8_t frame[SPI_LINK_FRAME_SIZE],
                              uint8_t seq,
                              uint32_t uptime_ms,
                              uint8_t role,
                              uint16_t err_flags)
{
    if (frame == NULL) {
        return;
    }

    spi_link_frame_clear(frame);
    frame[0] = (uint8_t)(SPI_LINK_MAGIC & 0xFFU);
    frame[1] = (uint8_t)((SPI_LINK_MAGIC >> 8) & 0xFFU);
    frame[2] = SPI_LINK_VER;
    frame[3] = seq;
    frame[4] = SPI_LINK_MSG_HEARTBEAT;
    frame[5] = 0U;
    frame[6] = 8U;
    frame[7] = 0U;

    frame[8] = (uint8_t)(uptime_ms & 0xFFU);
    frame[9] = (uint8_t)((uptime_ms >> 8) & 0xFFU);
    frame[10] = (uint8_t)((uptime_ms >> 16) & 0xFFU);
    frame[11] = (uint8_t)((uptime_ms >> 24) & 0xFFU);
    frame[12] = role;
    frame[13] = SPI_LINK_VER;
    frame[14] = (uint8_t)(err_flags & 0xFFU);
    frame[15] = (uint8_t)((err_flags >> 8) & 0xFFU);

    spi_link_frame_set_crc(frame);
}

bool_t spi_link_parse_heartbeat(const uint8_t frame[SPI_LINK_FRAME_SIZE],
                                spi_link_heartbeat_t *out)
{
    spi_link_hdr_t hdr;

    if ((frame == NULL) || (out == NULL)) {
        return FALSE;
    }
    if (spi_link_frame_parse_hdr(frame, &hdr) == FALSE) {
        return FALSE;
    }
    if (hdr.msg_id != SPI_LINK_MSG_HEARTBEAT) {
        return FALSE;
    }
    if (hdr.len < 8U) {
        return FALSE;
    }

    out->uptime_ms = (uint32_t)frame[8] | ((uint32_t)frame[9] << 8) |
                     ((uint32_t)frame[10] << 16) | ((uint32_t)frame[11] << 24);
    out->role = frame[12];
    out->proto_ver = frame[13];
    out->err_flags = (uint16_t)frame[14] | ((uint16_t)frame[15] << 8);
    return TRUE;
}
