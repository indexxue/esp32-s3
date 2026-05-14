#include "usb_serial_jtag.h"

#include <stddef.h>

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

static bool_t s_usbSerialJtagInited = FALSE;
static esp_err_t s_usbSerialJtagLastErr = ESP_OK;

static bool_t usbSerialJtagSetLastErr(esp_err_t err)
{
    s_usbSerialJtagLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

static TickType_t usbSerialJtagTimeoutMsToTicks(s32_t timeoutMs)
{
    if (timeoutMs < 0) {
        return portMAX_DELAY;
    }
    if (timeoutMs == 0) {
        return 0;
    }
    return pdMS_TO_TICKS((u32_t)timeoutMs);
}

bool_t UsbSerialJtagDriverInit(const UsbSerialJtagDriverConfig_t *config)
{
    usb_serial_jtag_driver_config_t idfCfg = {0};
    esp_err_t ret = ESP_OK;

    if (s_usbSerialJtagInited == TRUE) {
        return usbSerialJtagSetLastErr(ESP_OK);
    }
    if (config == NULL) {
        return usbSerialJtagSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if ((config->txBufferSize == 0U) || (config->rxBufferSize < USB_SERIAL_JTAG_RX_BUFFER_MIN)) {
        return usbSerialJtagSetLastErr(ESP_ERR_INVALID_ARG);
    }

    idfCfg.tx_buffer_size = config->txBufferSize;
    idfCfg.rx_buffer_size = config->rxBufferSize;

    ret = usb_serial_jtag_driver_install(&idfCfg);
    if (ret != ESP_OK) {
        return usbSerialJtagSetLastErr(ret);
    }

    s_usbSerialJtagInited = TRUE;
    return usbSerialJtagSetLastErr(ESP_OK);
}

bool_t UsbSerialJtagDriverDeinit(void)
{
    esp_err_t ret = ESP_OK;

    if (s_usbSerialJtagInited == FALSE) {
        return usbSerialJtagSetLastErr(ESP_OK);
    }

    ret = usb_serial_jtag_driver_uninstall();
    if (ret != ESP_OK) {
        return usbSerialJtagSetLastErr(ret);
    }

    s_usbSerialJtagInited = FALSE;
    return usbSerialJtagSetLastErr(ESP_OK);
}

bool_t UsbSerialJtagWrite(const u8_t *txBuffer, usize_t txLength, s32_t timeoutMs, usize_t *writtenLength)
{
    int n = 0;

    if (s_usbSerialJtagInited == FALSE) {
        return usbSerialJtagSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if ((txBuffer == NULL) || (txLength == 0U) || (writtenLength == NULL)) {
        return usbSerialJtagSetLastErr(ESP_ERR_INVALID_ARG);
    }

    n = usb_serial_jtag_write_bytes(txBuffer, (size_t)txLength, usbSerialJtagTimeoutMsToTicks(timeoutMs));
    if (n <= 0) {
        *writtenLength = 0U;
        return usbSerialJtagSetLastErr(ESP_FAIL);
    }

    *writtenLength = (usize_t)n;
    if (UsbSerialJtagWaitTxDone(timeoutMs) == FALSE) {
        return FALSE;
    }

    return usbSerialJtagSetLastErr(ESP_OK);
}

bool_t UsbSerialJtagRead(u8_t *rxBuffer, usize_t rxLength, s32_t timeoutMs, usize_t *readLength)
{
    int n = 0;

    if (s_usbSerialJtagInited == FALSE) {
        return usbSerialJtagSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if ((rxBuffer == NULL) || (rxLength == 0U) || (readLength == NULL)) {
        return usbSerialJtagSetLastErr(ESP_ERR_INVALID_ARG);
    }

    n = usb_serial_jtag_read_bytes(rxBuffer, (uint32_t)rxLength, usbSerialJtagTimeoutMsToTicks(timeoutMs));
    if (n < 0) {
        *readLength = 0U;
        return usbSerialJtagSetLastErr(ESP_FAIL);
    }

    *readLength = (usize_t)n;
    return usbSerialJtagSetLastErr(ESP_OK);
}

bool_t UsbSerialJtagWaitTxDone(s32_t timeoutMs)
{
    esp_err_t ret = ESP_OK;

    if (s_usbSerialJtagInited == FALSE) {
        return usbSerialJtagSetLastErr(ESP_ERR_INVALID_STATE);
    }

    ret = usb_serial_jtag_wait_tx_done(usbSerialJtagTimeoutMsToTicks(timeoutMs));
    return usbSerialJtagSetLastErr(ret);
}

bool_t UsbSerialJtagIsHostConnected(void)
{
    if (s_usbSerialJtagInited == FALSE) {
        return FALSE;
    }

    return usb_serial_jtag_is_connected() ? TRUE : FALSE;
}

s32_t UsbSerialJtagGetLastError(void)
{
    return (s32_t)s_usbSerialJtagLastErr;
}
