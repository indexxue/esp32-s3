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

static void spi_link_put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void spi_link_put_i16_le(uint8_t *p, int16_t v)
{
    spi_link_put_u16_le(p, (uint16_t)v);
}

static void spi_link_put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static void spi_link_frame_begin(uint8_t frame[SPI_LINK_FRAME_SIZE],
                                 uint8_t seq,
                                 uint8_t msg_id,
                                 uint8_t len)
{
    spi_link_frame_clear(frame);
    frame[0] = (uint8_t)(SPI_LINK_MAGIC & 0xFFU);
    frame[1] = (uint8_t)((SPI_LINK_MAGIC >> 8) & 0xFFU);
    frame[2] = SPI_LINK_VER;
    frame[3] = seq;
    frame[4] = msg_id;
    frame[5] = 0U;
    frame[6] = len;
    frame[7] = 0U;
}

void spi_link_build_detect_result(uint8_t frame[SPI_LINK_FRAME_SIZE],
                                  uint8_t seq,
                                  const spi_link_detect_result_t *det)
{
    uint8_t count;
    uint8_t i;
    uint8_t len;
    uint8_t *p;

    if ((frame == NULL) || (det == NULL)) {
        return;
    }

    count = det->count;
    if (count > SPI_LINK_DETECT_BOX_MAX) {
        count = SPI_LINK_DETECT_BOX_MAX;
    }
    len = (uint8_t)(6U + (8U * count));

    spi_link_frame_begin(frame, seq, SPI_LINK_MSG_DETECT_RESULT, len);
    p = &frame[8];
    spi_link_put_u16_le(&p[0], det->frame_w);
    spi_link_put_u16_le(&p[2], det->frame_h);
    p[4] = count;
    p[5] = (count == 0U) ? 0xFFU : det->best_index;

    for (i = 0U; i < count; i++) {
        uint8_t *b = &p[6U + (8U * i)];
        spi_link_put_u16_le(&b[0], det->box[i].x);
        spi_link_put_u16_le(&b[2], det->box[i].y);
        spi_link_put_u16_le(&b[4], det->box[i].w);
        b[6] = det->box[i].score_u8;
        b[7] = det->box[i].class_id;
    }

    spi_link_frame_set_crc(frame);
}

void spi_link_build_servo_telemetry(uint8_t frame[SPI_LINK_FRAME_SIZE],
                                    uint8_t seq,
                                    const spi_link_servo_telemetry_t *tel)
{
    uint8_t *p;

    if ((frame == NULL) || (tel == NULL)) {
        return;
    }

    spi_link_frame_begin(frame, seq, SPI_LINK_MSG_SERVO_TELEMETRY, 16U);
    p = &frame[8];
    spi_link_put_i16_le(&p[0], tel->pan_deg_x100);
    spi_link_put_i16_le(&p[2], tel->tilt_deg_x100);
    spi_link_put_u16_le(&p[4], tel->pan_pulse_us);
    spi_link_put_u16_le(&p[6], tel->tilt_pulse_us);
    spi_link_put_i16_le(&p[8], tel->pan_min_x100);
    spi_link_put_i16_le(&p[10], tel->pan_max_x100);
    spi_link_put_i16_le(&p[12], tel->tilt_min_x100);
    spi_link_put_i16_le(&p[14], tel->tilt_max_x100);
    spi_link_frame_set_crc(frame);
}

void spi_link_build_ctrl_ack(uint8_t frame[SPI_LINK_FRAME_SIZE],
                             uint8_t seq,
                             const spi_link_ctrl_ack_t *ack)
{
    uint8_t *p;

    if ((frame == NULL) || (ack == NULL)) {
        return;
    }

    spi_link_frame_begin(frame, seq, SPI_LINK_MSG_CTRL_ACK, 8U);
    p = &frame[8];
    p[0] = ack->sub_cmd;
    p[1] = ack->req_id;
    p[2] = ack->result;
    p[3] = 0U;
    spi_link_put_u32_le(&p[4], ack->detail);
    spi_link_frame_set_crc(frame);
}

bool_t spi_link_parse_ctrl_cmd(const uint8_t frame[SPI_LINK_FRAME_SIZE],
                               spi_link_ctrl_cmd_t *out)
{
    spi_link_hdr_t hdr;
    uint8_t argc;
    uint8_t i;

    if ((frame == NULL) || (out == NULL)) {
        return FALSE;
    }
    if (spi_link_frame_parse_hdr(frame, &hdr) == FALSE) {
        return FALSE;
    }
    if (hdr.msg_id != SPI_LINK_MSG_CTRL_CMD) {
        return FALSE;
    }
    if (hdr.len < 4U) {
        return FALSE;
    }

    argc = frame[10];
    if ((uint16_t)argc > (uint16_t)(hdr.len - 4U)) {
        return FALSE;
    }
    if (argc > (uint8_t)sizeof(out->args)) {
        return FALSE;
    }

    out->sub_cmd = frame[8];
    out->req_id = frame[9];
    out->argc = argc;
    for (i = 0U; i < argc; i++) {
        out->args[i] = frame[12U + i];
    }
    for (; i < (uint8_t)sizeof(out->args); i++) {
        out->args[i] = 0U;
    }
    return TRUE;
}
