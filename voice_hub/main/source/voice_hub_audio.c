#include "voice_hub_audio.h"

#include "voice_hub_config.h"

#include "board.h"
#include "es8311.h"
#include "gpio.h"
#include "i2c.h"
#include "log.h"

static es8311_t s_es8311;
static bool_t s_audio_ready;

#if VOICE_HUB_ENABLE_AUDIO
static bool_t s_pa_gpio_ready;

static status_t voice_hub_audio_pa_gpio_init(void)
{
    GpioPinConfig_t cfg = {0};

    if (BOARD_ES8311_PIN_PA_EN < 0) {
        LOG_WARN("NS4150B PA_EN pin not configured");
        return STATUS_FAIL;
    }

    cfg.pin        = (s32_t)BOARD_ES8311_PIN_PA_EN;
    cfg.mode       = GPIO_MODE_OUTPUT_E;
    cfg.pullUpEn   = GPIO_PULL_DISABLE_E;
    cfg.pullDownEn = GPIO_PULL_DISABLE_E;
    cfg.intrType   = GPIO_INTR_DISABLE_E;

    if (GpioDriverInit() != TRUE) {
        LOG_ERROR("PA_EN: GpioDriverInit failed");
        return STATUS_FAIL;
    }
    if (GpioConfigurePin(&cfg) != TRUE) {
        LOG_ERROR("PA_EN: GpioConfigurePin GPIO%d failed", BOARD_ES8311_PIN_PA_EN);
        return STATUS_FAIL;
    }

    s_pa_gpio_ready = TRUE;
    (void)voice_hub_audio_pa_set(FALSE);
    LOG_INFO("NS4150B PA_EN on GPIO%d (active=%d)", BOARD_ES8311_PIN_PA_EN, BOARD_ES8311_PA_EN_ACTIVE_LEVEL);
    return STATUS_OK;
}

status_t voice_hub_audio_pa_set(bool_t enable)
{
    u32_t level;

    if (!s_pa_gpio_ready || BOARD_ES8311_PIN_PA_EN < 0) {
        return STATUS_FAIL;
    }

    level = (enable == TRUE) ? (u32_t)BOARD_ES8311_PA_EN_ACTIVE_LEVEL
                             : (u32_t)(BOARD_ES8311_PA_EN_ACTIVE_LEVEL ? 0U : 1U);
    if (GpioWritePin((s32_t)BOARD_ES8311_PIN_PA_EN, level) != TRUE) {
        return STATUS_FAIL;
    }
    return STATUS_OK;
}

static int voice_hub_es8311_i2c_write(uint8_t addr7, const uint8_t *data, uint16_t len)
{
    if (I2cWrite((s32_t)BOARD_I2C_ES8311_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int voice_hub_es8311_i2c_read(uint8_t addr7, uint8_t *data, uint16_t len)
{
    if (I2cRead((s32_t)BOARD_I2C_ES8311_PORT, (u16_t)addr7, data, (usize_t)len) != TRUE) {
        return -1;
    }
    return 0;
}

static int voice_hub_es8311_i2c_write_read(uint8_t addr7,
                                             const uint8_t *write_data,
                                             uint16_t write_len,
                                             uint8_t *read_data,
                                             uint16_t read_len)
{
    if (I2cWriteRead((s32_t)BOARD_I2C_ES8311_PORT,
                     (u16_t)addr7,
                     write_data,
                     (usize_t)write_len,
                     read_data,
                     (usize_t)read_len) != TRUE) {
        return -1;
    }
    return 0;
}

#else /* !VOICE_HUB_ENABLE_AUDIO */

status_t voice_hub_audio_pa_set(bool_t enable)
{
    (void)enable;
    return STATUS_FAIL;
}

#endif /* VOICE_HUB_ENABLE_AUDIO */

status_t voice_hub_audio_init(void)
{
#if !VOICE_HUB_ENABLE_AUDIO
    LOG_INFO("voice_hub audio disabled (VOICE_HUB_ENABLE_AUDIO=0)");
    return STATUS_OK;
#else
    es8311_config_t cfg = {0};

    if (BOARD_I2C_BUS1_PIN_SCL < 0 || BOARD_I2C_BUS1_PIN_SDA < 0) {
        LOG_WARN("ES8311 I2C pins not configured in board.h");
        return STATUS_FAIL;
    }

    cfg.write           = voice_hub_es8311_i2c_write;
    cfg.read            = voice_hub_es8311_i2c_read;
    cfg.write_read      = voice_hub_es8311_i2c_write_read;
    cfg.i2c_addr7       = (uint8_t)BOARD_I2C_ES8311_ADDR;
    cfg.sample_rate_hz  = BOARD_ES8311_SAMPLE_RATE_HZ;
    cfg.i2s_port        = BOARD_ES8311_I2S_PORT;

    if (es8311_init_with_config(&s_es8311, &cfg) != ES8311_OK) {
        LOG_ERROR("es8311_init_with_config failed");
        return STATUS_FAIL;
    }

    if (voice_hub_audio_pa_gpio_init() != STATUS_OK) {
        LOG_WARN("NS4150B PA_EN init failed (non-fatal until playback)");
    }

    s_audio_ready = TRUE;
    LOG_INFO("ES8311 ready: MCLK=%d BCLK=%d WS=%d DIN=%d I2S_TX=%d PA_EN=%d",
             BOARD_ES8311_PIN_MCLK,
             BOARD_ES8311_PIN_BCLK,
             BOARD_ES8311_PIN_WS,
             BOARD_ES8311_PIN_DIN,
             (int)BOARD_ES8311_I2S_HAS_TX,
             BOARD_ES8311_PIN_PA_EN);
    return STATUS_OK;
#endif
}

bool_t voice_hub_audio_is_ready(void)
{
    return (s_audio_ready == TRUE) && es8311_is_initialized(&s_es8311) ? TRUE : FALSE;
}

status_t voice_hub_audio_play_prompt(const char *tag)
{
    (void)tag;
#if !VOICE_HUB_ENABLE_AUDIO
    return STATUS_FAIL;
#else
    if (!voice_hub_audio_is_ready()) {
        return STATUS_FAIL;
    }
    /* TODO(M2): 本地提示音 / WAV 播放；播放前使能 NS4150B。 */
    (void)voice_hub_audio_pa_set(TRUE);
    (void)es8311_set_mode(&s_es8311, ES8311_MODE_PLAYBACK);
    (void)es8311_start(&s_es8311);
    return STATUS_OK;
#endif
}

status_t voice_hub_audio_intercom_start(void)
{
#if !VOICE_HUB_ENABLE_INTERCOM
    return STATUS_FAIL;
#else
    if (!voice_hub_audio_is_ready()) {
        return STATUS_FAIL;
    }
    (void)voice_hub_audio_pa_set(TRUE);
    (void)es8311_set_mode(&s_es8311, ES8311_MODE_FULL_DUPLEX);
    return es8311_start(&s_es8311) == ES8311_OK ? STATUS_OK : STATUS_FAIL;
#endif
}

status_t voice_hub_audio_intercom_stop(void)
{
#if !VOICE_HUB_ENABLE_INTERCOM
    return STATUS_OK;
#else
    if (!voice_hub_audio_is_ready()) {
        return STATUS_OK;
    }
    (void)es8311_stop(&s_es8311);
    (void)voice_hub_audio_pa_set(FALSE);
    return STATUS_OK;
#endif
}
