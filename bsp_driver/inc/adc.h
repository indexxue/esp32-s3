#ifndef DRIVER_ADC_H
#define DRIVER_ADC_H

#include "type.h"

typedef enum {
    ADC_UNIT_1_E = 1,
    ADC_UNIT_2_E = 2
} AdcUnit_t;

typedef enum {
    ADC_CHANNEL_0_E = 0,
    ADC_CHANNEL_1_E = 1,
    ADC_CHANNEL_2_E = 2,
    ADC_CHANNEL_3_E = 3,
    ADC_CHANNEL_4_E = 4,
    ADC_CHANNEL_5_E = 5,
    ADC_CHANNEL_6_E = 6,
    ADC_CHANNEL_7_E = 7,
    ADC_CHANNEL_8_E = 8,
    ADC_CHANNEL_9_E = 9
} AdcChannel_t;

typedef enum {
    ADC_ATTEN_DB_0_E = 0,
    ADC_ATTEN_DB_2_5_E = 1,
    ADC_ATTEN_DB_6_E = 2,
    ADC_ATTEN_DB_12_E = 3
} AdcAtten_t;

typedef enum {
    ADC_BITWIDTH_DEFAULT_E = 0,
    ADC_BITWIDTH_9_E = 9,
    ADC_BITWIDTH_10_E = 10,
    ADC_BITWIDTH_11_E = 11,
    ADC_BITWIDTH_12_E = 12
} AdcBitWidth_t;

typedef struct {
    AdcUnit_t unit;
} AdcDriverConfig_t;

typedef struct {
    AdcChannel_t channel;
    AdcAtten_t atten;
    AdcBitWidth_t bitWidth;
} AdcChannelConfig_t;

bool_t AdcDriverInit(const AdcDriverConfig_t *config);
bool_t AdcDriverDeinit(void);

bool_t AdcConfigureChannel(const AdcChannelConfig_t *config);
bool_t AdcReadRaw(AdcChannel_t channel, s32_t *rawValue);
bool_t AdcReadVoltageMv(AdcChannel_t channel, s32_t *voltageMv);

s32_t AdcGetLastError(void);

#endif
