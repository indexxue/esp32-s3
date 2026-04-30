#ifndef DRIVER_UART_H
#define DRIVER_UART_H

#include "type.h"

typedef enum {
    UART_PORT_0_E = 0,
    UART_PORT_1_E = 1,
    UART_PORT_2_E = 2
} UartPort_t;

typedef enum {
    UART_DATA_BITS_5_E = 5,
    UART_DATA_BITS_6_E = 6,
    UART_DATA_BITS_7_E = 7,
    UART_DATA_BITS_8_E = 8
} UartDataBits_t;

typedef enum {
    UART_PARITY_NONE_E = 0,
    UART_PARITY_ODD_E = 1,
    UART_PARITY_EVEN_E = 2
} UartParity_t;

typedef enum {
    UART_STOP_BITS_1_E = 1,
    UART_STOP_BITS_1_5_E = 15,
    UART_STOP_BITS_2_E = 2
} UartStopBits_t;

typedef enum {
    UART_FLOW_CTRL_DISABLE_E = 0,
    UART_FLOW_CTRL_RTS_E = 1,
    UART_FLOW_CTRL_CTS_E = 2,
    UART_FLOW_CTRL_CTS_RTS_E = 3
} UartFlowCtrl_t;

typedef enum {
    UART_EVENT_DATA_E = 0,
    UART_EVENT_FIFO_OVF_E = 1,
    UART_EVENT_BUFFER_FULL_E = 2,
    UART_EVENT_BREAK_E = 3,
    UART_EVENT_PARITY_ERR_E = 4,
    UART_EVENT_FRAME_ERR_E = 5,
    UART_EVENT_PATTERN_DET_E = 6,
    UART_EVENT_UNKNOWN_E = 255
} UartEventType_t;

typedef struct {
    UartPort_t port;
    u32_t baudRate;
    UartDataBits_t dataBits;
    UartParity_t parity;
    UartStopBits_t stopBits;
    UartFlowCtrl_t flowCtrl;
    u8_t rxFlowCtrlThreshold;
    s32_t txPin;
    s32_t rxPin;
    s32_t rtsPin;
    s32_t ctsPin;
    s32_t txBufferSize;
    s32_t rxBufferSize;
    u8_t eventQueueSize;
    s32_t intrFlags;
} UartDriverConfig_t;

typedef struct {
    UartEventType_t type;
    usize_t dataSize;
} UartEvent_t;

bool_t UartDriverInit(const UartDriverConfig_t *config);
bool_t UartDriverDeinit(void);

bool_t UartSetBaudRate(u32_t baudRate);
bool_t UartWrite(const u8_t *txBuffer, usize_t txLength, s32_t timeoutMs, usize_t *writtenLength);
bool_t UartRead(u8_t *rxBuffer, usize_t rxLength, s32_t timeoutMs, usize_t *readLength);
bool_t UartReadBytesAvailable(usize_t *availableBytes);
bool_t UartWaitTxDone(s32_t timeoutMs);
bool_t UartFlush(void);
bool_t UartReadEvent(UartEvent_t *event, s32_t timeoutMs);

s32_t UartGetLastError(void);

#endif
