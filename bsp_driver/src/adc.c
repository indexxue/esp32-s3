#include "adc.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

/**
 * 无 efuse/曲线校准时，`adc_oneshot_read` 得到的是码值而非 mV。
 * 在 ADC_ATTEN_DB_12、12bit 量程下，用约 3100mV 满量程做线性近似（与 IDF 文档量级一致，略差于校准）。
 */
static int adc_uncali_raw_to_mv_db12(int raw)
{
    if (raw < 0) {
        raw = 0;
    }
    return (int)(((int64_t)raw * 3100) / 4095);
}

static bool_t s_adcDriverInited = FALSE;
static adc_unit_t s_adcUnit = ADC_UNIT_1;
static adc_oneshot_unit_handle_t s_adcHandle = NULL;
static adc_cali_handle_t s_adcCaliHandle = NULL;
static bool_t s_adcCaliEnabled = FALSE;
static esp_err_t s_adcLastErr = ESP_OK;

static bool_t adcSetLastErr(esp_err_t err)
{
    s_adcLastErr = err;
    return (err == ESP_OK) ? TRUE : FALSE;
}

static bool_t adcConvertUnit(AdcUnit_t unit, adc_unit_t *outUnit)
{
    if (outUnit == NULL) {
        return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (unit) {
        case ADC_UNIT_1_E: *outUnit = ADC_UNIT_1; break;
        case ADC_UNIT_2_E: *outUnit = ADC_UNIT_2; break;
        default: return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return adcSetLastErr(ESP_OK);
}

static bool_t adcConvertChannel(AdcChannel_t channel, adc_channel_t *outChannel)
{
    if (outChannel == NULL) {
        return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (channel) {
        case ADC_CHANNEL_0_E: *outChannel = ADC_CHANNEL_0; break;
        case ADC_CHANNEL_1_E: *outChannel = ADC_CHANNEL_1; break;
        case ADC_CHANNEL_2_E: *outChannel = ADC_CHANNEL_2; break;
        case ADC_CHANNEL_3_E: *outChannel = ADC_CHANNEL_3; break;
        case ADC_CHANNEL_4_E: *outChannel = ADC_CHANNEL_4; break;
        case ADC_CHANNEL_5_E: *outChannel = ADC_CHANNEL_5; break;
        case ADC_CHANNEL_6_E: *outChannel = ADC_CHANNEL_6; break;
        case ADC_CHANNEL_7_E: *outChannel = ADC_CHANNEL_7; break;
        case ADC_CHANNEL_8_E: *outChannel = ADC_CHANNEL_8; break;
        case ADC_CHANNEL_9_E: *outChannel = ADC_CHANNEL_9; break;
        default: return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return adcSetLastErr(ESP_OK);
}

static bool_t adcConvertAtten(AdcAtten_t atten, adc_atten_t *outAtten)
{
    if (outAtten == NULL) {
        return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (atten) {
        case ADC_ATTEN_DB_0_E: *outAtten = ADC_ATTEN_DB_0; break;
        case ADC_ATTEN_DB_2_5_E: *outAtten = ADC_ATTEN_DB_2_5; break;
        case ADC_ATTEN_DB_6_E: *outAtten = ADC_ATTEN_DB_6; break;
        case ADC_ATTEN_DB_12_E: *outAtten = ADC_ATTEN_DB_12; break;
        default: return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return adcSetLastErr(ESP_OK);
}

static bool_t adcConvertBitWidth(AdcBitWidth_t bitWidth, adc_bitwidth_t *outBitWidth)
{
    if (outBitWidth == NULL) {
        return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    switch (bitWidth) {
        case ADC_BITWIDTH_DEFAULT_E: *outBitWidth = ADC_BITWIDTH_DEFAULT; break;
        case ADC_BITWIDTH_9_E: *outBitWidth = ADC_BITWIDTH_9; break;
        case ADC_BITWIDTH_10_E: *outBitWidth = ADC_BITWIDTH_10; break;
        case ADC_BITWIDTH_11_E: *outBitWidth = ADC_BITWIDTH_11; break;
        case ADC_BITWIDTH_12_E: *outBitWidth = ADC_BITWIDTH_12; break;
        default: return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    return adcSetLastErr(ESP_OK);
}

static bool_t adcCreateCalibration(void)
{
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t caliConfig = {
        .unit_id = s_adcUnit,
        .chan = ADC_CHANNEL_0,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&caliConfig, &s_adcCaliHandle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t caliConfig = {
        .unit_id = s_adcUnit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&caliConfig, &s_adcCaliHandle);
#endif

    if (ret == ESP_OK) {
        s_adcCaliEnabled = TRUE;
        return adcSetLastErr(ESP_OK);
    }

    if ((ret == ESP_ERR_NOT_SUPPORTED) || (ret == ESP_ERR_NOT_FOUND)) {
        s_adcCaliEnabled = FALSE;
        s_adcCaliHandle = NULL;
        return adcSetLastErr(ESP_OK);
    }

    return adcSetLastErr(ret);
}

bool_t AdcDriverInit(const AdcDriverConfig_t *config)
{
    esp_err_t ret = ESP_OK;
    adc_oneshot_unit_init_cfg_t initConfig = {0};
    adc_unit_t unit = ADC_UNIT_1;

    if (s_adcDriverInited) {
        return adcSetLastErr(ESP_OK);
    }
    if (config == NULL) {
        return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }

    if (adcConvertUnit(config->unit, &unit) == FALSE) {
        return FALSE;
    }

    s_adcUnit = unit;
    initConfig.unit_id = s_adcUnit;
    initConfig.ulp_mode = ADC_ULP_MODE_DISABLE;

    ret = adc_oneshot_new_unit(&initConfig, &s_adcHandle);
    if (ret != ESP_OK) {
        return adcSetLastErr(ret);
    }

    if (adcCreateCalibration() == FALSE) {
        (void)adc_oneshot_del_unit(s_adcHandle);
        s_adcHandle = NULL;
        return FALSE;
    }

    s_adcDriverInited = TRUE;
    return adcSetLastErr(ESP_OK);
}

bool_t AdcDriverDeinit(void)
{
    esp_err_t ret = ESP_OK;

    if (!s_adcDriverInited) {
        return adcSetLastErr(ESP_OK);
    }

    if (s_adcCaliEnabled == TRUE) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        ret = adc_cali_delete_scheme_curve_fitting(s_adcCaliHandle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        ret = adc_cali_delete_scheme_line_fitting(s_adcCaliHandle);
#else
        ret = ESP_OK;
#endif
        if (ret != ESP_OK) {
            return adcSetLastErr(ret);
        }
        s_adcCaliEnabled = FALSE;
        s_adcCaliHandle = NULL;
    }

    ret = adc_oneshot_del_unit(s_adcHandle);
    if (ret != ESP_OK) {
        return adcSetLastErr(ret);
    }

    s_adcHandle = NULL;
    s_adcDriverInited = FALSE;
    return adcSetLastErr(ESP_OK);
}

bool_t AdcConfigureChannel(const AdcChannelConfig_t *config)
{
    adc_oneshot_chan_cfg_t chanConfig = {0};
    adc_channel_t channel = ADC_CHANNEL_0;
    esp_err_t ret = ESP_OK;

    if (!s_adcDriverInited) {
        return adcSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (config == NULL) {
        return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (adcConvertChannel(config->channel, &channel) == FALSE) {
        return FALSE;
    }
    if (adcConvertAtten(config->atten, &chanConfig.atten) == FALSE) {
        return FALSE;
    }
    if (adcConvertBitWidth(config->bitWidth, &chanConfig.bitwidth) == FALSE) {
        return FALSE;
    }

    ret = adc_oneshot_config_channel(s_adcHandle, channel, &chanConfig);
    return adcSetLastErr(ret);
}

bool_t AdcReadRaw(AdcChannel_t channel, s32_t *rawValue)
{
    esp_err_t ret = ESP_OK;
    adc_channel_t channelId = ADC_CHANNEL_0;
    int raw = 0;

    if (!s_adcDriverInited) {
        return adcSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (rawValue == NULL) {
        return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (adcConvertChannel(channel, &channelId) == FALSE) {
        return FALSE;
    }

    ret = adc_oneshot_read(s_adcHandle, channelId, &raw);
    if (ret != ESP_OK) {
        return adcSetLastErr(ret);
    }

    *rawValue = (s32_t)raw;
    return adcSetLastErr(ESP_OK);
}

bool_t AdcReadVoltageMv(AdcChannel_t channel, s32_t *voltageMv)
{
    esp_err_t ret = ESP_OK;
    adc_channel_t channelId = ADC_CHANNEL_0;
    int raw = 0;
    int voltage = 0;

    if (!s_adcDriverInited) {
        return adcSetLastErr(ESP_ERR_INVALID_STATE);
    }
    if (voltageMv == NULL) {
        return adcSetLastErr(ESP_ERR_INVALID_ARG);
    }
    if (adcConvertChannel(channel, &channelId) == FALSE) {
        return FALSE;
    }

    ret = adc_oneshot_read(s_adcHandle, channelId, &raw);
    if (ret != ESP_OK) {
        return adcSetLastErr(ret);
    }

    if (s_adcCaliEnabled == TRUE) {
        ret = adc_cali_raw_to_voltage(s_adcCaliHandle, raw, &voltage);
        if (ret != ESP_OK) {
            return adcSetLastErr(ret);
        }
        *voltageMv = (s32_t)voltage;
    } else {
        *voltageMv = (s32_t)adc_uncali_raw_to_mv_db12(raw);
    }

    return adcSetLastErr(ESP_OK);
}

s32_t AdcGetLastError(void)
{
    return (s32_t)s_adcLastErr;
}
