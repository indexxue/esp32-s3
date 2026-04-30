#include "uart.h"

#include <stddef.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static bool_t s_uartDriverInited = FALSE;
static uart_port_t s_uartPort = UART_NUM_0;
static QueueHandle_t s_uartEventQueue = NULL;
static esp_err_t s_uartLastErr = ESP_OK;

static bool_t uartSetLastErr(esp_err_t err)
{
    s_uartLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

static bool_t uartConvertPort(UartPort_t port, uart_port_t *outPort)
{
    if (outPort == NULL) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    switch (port) {
        case UART_PORT_0_E: *outPort = UART_NUM_0; break;
        case UART_PORT_1_E: *outPort = UART_NUM_1; break;
#if SOC_UART_NUM > 2
        case UART_PORT_2_E: *outPort = UART_NUM_2; break;
#endif
        default: return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return uartSetLastErr(ESP_OK);
}

static bool_t uartConvertDataBits(UartDataBits_t dataBits, uart_word_length_t *outDataBits)
{
    if (outDataBits == NULL) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    switch (dataBits) {
        case UART_DATA_BITS_5_E: *outDataBits = UART_DATA_5_BITS; break;
        case UART_DATA_BITS_6_E: *outDataBits = UART_DATA_6_BITS; break;
        case UART_DATA_BITS_7_E: *outDataBits = UART_DATA_7_BITS; break;
        case UART_DATA_BITS_8_E: *outDataBits = UART_DATA_8_BITS; break;
        default: return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return uartSetLastErr(ESP_OK);
}

static bool_t uartConvertParity(UartParity_t parity, uart_parity_t *outParity)
{
    if (outParity == NULL) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    switch (parity) {
        case UART_PARITY_NONE_E: *outParity = UART_PARITY_DISABLE; break;
        case UART_PARITY_ODD_E: *outParity = UART_PARITY_ODD; break;
        case UART_PARITY_EVEN_E: *outParity = UART_PARITY_EVEN; break;
        default: return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return uartSetLastErr(ESP_OK);
}

static bool_t uartConvertStopBits(UartStopBits_t stopBits, uart_stop_bits_t *outStopBits)
{
    if (outStopBits == NULL) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    switch (stopBits) {
        case UART_STOP_BITS_1_E: *outStopBits = UART_STOP_BITS_1; break;
        case UART_STOP_BITS_1_5_E: *outStopBits = UART_STOP_BITS_1_5; break;
        case UART_STOP_BITS_2_E: *outStopBits = UART_STOP_BITS_2; break;
        default: return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return uartSetLastErr(ESP_OK);
}

static bool_t uartConvertFlowCtrl(UartFlowCtrl_t flowCtrl, uart_hw_flowcontrol_t *outFlowCtrl)
{
    if (outFlowCtrl == NULL) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    switch (flowCtrl) {
        case UART_FLOW_CTRL_DISABLE_E: *outFlowCtrl = UART_HW_FLOWCTRL_DISABLE; break;
        case UART_FLOW_CTRL_RTS_E: *outFlowCtrl = UART_HW_FLOWCTRL_RTS; break;
        case UART_FLOW_CTRL_CTS_E: *outFlowCtrl = UART_HW_FLOWCTRL_CTS; break;
        case UART_FLOW_CTRL_CTS_RTS_E: *outFlowCtrl = UART_HW_FLOWCTRL_CTS_RTS; break;
        default: return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    return uartSetLastErr(ESP_OK);
}

static TickType_t uartTimeoutMsToTicks(s32_t timeoutMs)
{
    if (timeoutMs < 0) {
        return portMAX_DELAY;
    }
    if (timeoutMs == 0) {
        return 0;
    }
    return pdMS_TO_TICKS((u32_t)timeoutMs);
}

bool_t UartDriverInit(const UartDriverConfig_t *config)
{
    uart_config_t uartConfig = {0};
    uart_word_length_t dataBits = UART_DATA_8_BITS;
    uart_parity_t parity = UART_PARITY_DISABLE;
    uart_stop_bits_t stopBits = UART_STOP_BITS_1;
    uart_hw_flowcontrol_t flowCtrl = UART_HW_FLOWCTRL_DISABLE;
    uart_port_t port = UART_NUM_0;
    esp_err_t ret = ESP_OK;

    if (s_uartDriverInited == TRUE) {
        return uartSetLastErr(ESP_OK);
    }
    if (config == NULL) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if ((config->baudRate == 0U) || (config->txBufferSize <= 0) || (config->rxBufferSize <= 0) ||
        (config->eventQueueSize == 0U)) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    if (uartConvertPort(config->port, &port) == FALSE) {
        return FALSE;
    }
    if (uartConvertDataBits(config->dataBits, &dataBits) == FALSE) {
        return FALSE;
    }
    if (uartConvertParity(config->parity, &parity) == FALSE) {
        return FALSE;
    }
    if (uartConvertStopBits(config->stopBits, &stopBits) == FALSE) {
        return FALSE;
    }
    if (uartConvertFlowCtrl(config->flowCtrl, &flowCtrl) == FALSE) {
        return FALSE;
    }

    uartConfig.baud_rate = (s32_t)config->baudRate;
    uartConfig.data_bits = dataBits;
    uartConfig.parity = parity;
    uartConfig.stop_bits = stopBits;
    uartConfig.flow_ctrl = flowCtrl;
    uartConfig.rx_flow_ctrl_thresh = config->rxFlowCtrlThreshold;
    uartConfig.source_clk = UART_SCLK_DEFAULT;

    ret = uart_driver_install(port,
                              config->rxBufferSize,
                              config->txBufferSize,
                              config->eventQueueSize,
                              &s_uartEventQueue,
                              config->intrFlags);
    if (ret != ESP_OK) {
        return uartSetLastErr(ret);
    }

    ret = uart_param_config(port, &uartConfig);
    if (ret != ESP_OK) {
        (void)uart_driver_delete(port);
        s_uartEventQueue = NULL;
        return uartSetLastErr(ret);
    }

    ret = uart_set_pin(port, config->txPin, config->rxPin, config->rtsPin, config->ctsPin);
    if (ret != ESP_OK) {
        (void)uart_driver_delete(port);
        s_uartEventQueue = NULL;
        return uartSetLastErr(ret);
    }

    s_uartPort = port;
    s_uartDriverInited = TRUE;
    return uartSetLastErr(ESP_OK);
}

bool_t UartDriverDeinit(void)
{
    esp_err_t ret = ESP_OK;

    if (s_uartDriverInited == FALSE) {
        return uartSetLastErr(ESP_OK);
    }

    ret = uart_driver_delete(s_uartPort);
    if (ret != ESP_OK) {
        return uartSetLastErr(ret);
    }

    s_uartEventQueue = NULL;
    s_uartPort = UART_NUM_0;
    s_uartDriverInited = FALSE;
    return uartSetLastErr(ESP_OK);
}

bool_t UartSetBaudRate(u32_t baudRate)
{
    esp_err_t ret = ESP_OK;

    if (s_uartDriverInited == FALSE) {
        return uartSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (baudRate == 0U) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    ret = uart_set_baudrate(s_uartPort, baudRate);
    return uartSetLastErr(ret);
}

bool_t UartWrite(const u8_t *txBuffer, usize_t txLength, s32_t timeoutMs, usize_t *writtenLength)
{
    s32_t retLen = 0;

    if (s_uartDriverInited == FALSE) {
        return uartSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if ((txBuffer == NULL) || (txLength == 0U) || (writtenLength == NULL)) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    retLen = uart_write_bytes(s_uartPort, txBuffer, txLength);
    if (retLen < 0) {
        *writtenLength = 0U;
        return uartSetLastErr(ESP_FAIL);
    }

    *writtenLength = (usize_t)retLen;
    if (UartWaitTxDone(timeoutMs) == FALSE) {
        return FALSE;
    }

    return uartSetLastErr(ESP_OK);
}

bool_t UartRead(u8_t *rxBuffer, usize_t rxLength, s32_t timeoutMs, usize_t *readLength)
{
    s32_t retLen = 0;

    if (s_uartDriverInited == FALSE) {
        return uartSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if ((rxBuffer == NULL) || (rxLength == 0U) || (readLength == NULL)) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    retLen = uart_read_bytes(s_uartPort, rxBuffer, rxLength, uartTimeoutMsToTicks(timeoutMs));
    if (retLen < 0) {
        *readLength = 0U;
        return uartSetLastErr(ESP_FAIL);
    }

    *readLength = (usize_t)retLen;
    return uartSetLastErr(ESP_OK);
}

bool_t UartReadBytesAvailable(usize_t *availableBytes)
{
    usize_t bufferedLength = 0U;
    esp_err_t ret = ESP_OK;

    if (s_uartDriverInited == FALSE) {
        return uartSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (availableBytes == NULL) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }

    ret = uart_get_buffered_data_len(s_uartPort, &bufferedLength);
    if (ret != ESP_OK) {
        return uartSetLastErr(ret);
    }

    *availableBytes = bufferedLength;
    return uartSetLastErr(ESP_OK);
}

bool_t UartWaitTxDone(s32_t timeoutMs)
{
    esp_err_t ret = ESP_OK;

    if (s_uartDriverInited == FALSE) {
        return uartSetLastErr(ESP_ERR_INVALID_STATE);
    }

    ret = uart_wait_tx_done(s_uartPort, uartTimeoutMsToTicks(timeoutMs));
    return uartSetLastErr(ret);
}

bool_t UartFlush(void)
{
    esp_err_t ret = ESP_OK;

    if (s_uartDriverInited == FALSE) {
        return uartSetLastErr(ESP_ERR_INVALID_STATE);
    }

    ret = uart_flush_input(s_uartPort);
    return uartSetLastErr(ret);
}

bool_t UartReadEvent(UartEvent_t *event, s32_t timeoutMs)
{
    uart_event_t uartEvent = {0};

    if (s_uartDriverInited == FALSE) {
        return uartSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (event == NULL) {
        return uartSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (s_uartEventQueue == NULL) {
        return uartSetLastErr(ESP_ERR_INVALID_STATE);
    }

    if (xQueueReceive(s_uartEventQueue, &uartEvent, uartTimeoutMsToTicks(timeoutMs)) != pdTRUE) {
        return uartSetLastErr(ESP_ERR_TIMEOUT);
    }

    switch (uartEvent.type) {
        case UART_DATA: event->type = UART_EVENT_DATA_E; break;
        case UART_FIFO_OVF: event->type = UART_EVENT_FIFO_OVF_E; break;
        case UART_BUFFER_FULL: event->type = UART_EVENT_BUFFER_FULL_E; break;
        case UART_BREAK: event->type = UART_EVENT_BREAK_E; break;
        case UART_PARITY_ERR: event->type = UART_EVENT_PARITY_ERR_E; break;
        case UART_FRAME_ERR: event->type = UART_EVENT_FRAME_ERR_E; break;
        case UART_PATTERN_DET: event->type = UART_EVENT_PATTERN_DET_E; break;
        default: event->type = UART_EVENT_UNKNOWN_E; break;
    }

    event->dataSize = (usize_t)uartEvent.size;
    return uartSetLastErr(ESP_OK);
}

s32_t UartGetLastError(void)
{
    return (s32_t)s_uartLastErr;
}
