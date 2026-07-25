/**
 * @file camera_spi_host.h
 * @brief Camera SPI Master：与小车 MCU（Slave）32B 轮询；L1 HEARTBEAT。
 *        CAMERA_SPI_WIRE_TEST=0 为正式协议；仅 L0 联调时临时置 1。
 */

#ifndef CAMERA_SPI_HOST_H
#define CAMERA_SPI_HOST_H

#include "type.h"

status_t camera_spi_host_start(void);

#endif /* CAMERA_SPI_HOST_H */
