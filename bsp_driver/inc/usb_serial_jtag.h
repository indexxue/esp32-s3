#ifndef DRIVER_USB_SERIAL_JTAG_H
#define DRIVER_USB_SERIAL_JTAG_H

#include "type.h"

/** ESP-IDF 要求 RX 环形缓冲大于 64 字节（见 usb_serial_jtag_driver_install） */
#define USB_SERIAL_JTAG_RX_BUFFER_MIN (65U)

typedef struct {
    u32_t txBufferSize;
    u32_t rxBufferSize;
} UsbSerialJtagDriverConfig_t;

bool_t UsbSerialJtagDriverInit(const UsbSerialJtagDriverConfig_t *config);
bool_t UsbSerialJtagDriverDeinit(void);

bool_t UsbSerialJtagWrite(const u8_t *txBuffer, usize_t txLength, s32_t timeoutMs, usize_t *writtenLength);
bool_t UsbSerialJtagRead(u8_t *rxBuffer, usize_t rxLength, s32_t timeoutMs, usize_t *readLength);
bool_t UsbSerialJtagWaitTxDone(s32_t timeoutMs);

/** 与主机建立 USB 连接（收到 SOF）时为 TRUE，具体语义见 IDF usb_serial_jtag_is_connected */
bool_t UsbSerialJtagIsHostConnected(void);

s32_t UsbSerialJtagGetLastError(void);

#endif
