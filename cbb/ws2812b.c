/**
 * @file    ws2812b.c
 * @brief   WS2812B RMT 驱动实现；编码器时序逻辑参考 ESP-IDF examples/peripherals/rmt/led_strip（Apache-2.0）。
 */

#include "ws2812b.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ws2812b";

/* 与 IDF led_strip 示例中 __containerof 等价，避免依赖具体 libc 宏名 */
#define WS2812B_CONTAINEROF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct
{
    rmt_encoder_t        base;
    rmt_encoder_t       *bytes_encoder;
    rmt_encoder_t       *copy_encoder;
    int                  state;
    rmt_symbol_word_t    reset_code;
} ws2812b_strip_encoder_t;

RMT_ENCODER_FUNC_ATTR
static size_t ws2812b_encode_strip(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
    const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state)
{
    ws2812b_strip_encoder_t *led_encoder = WS2812B_CONTAINEROF(encoder, ws2812b_strip_encoder_t, base);
    rmt_encoder_handle_t      bytes_encoder = led_encoder->bytes_encoder;
    rmt_encoder_handle_t      copy_encoder  = led_encoder->copy_encoder;
    rmt_encode_state_t        session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t        state         = RMT_ENCODING_RESET;
    size_t                    encoded_symbols = 0;

    switch (led_encoder->state)
    {
    case 0:
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE)
        {
            led_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL)
        {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        /* fall-through */
    case 1:
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_encoder->reset_code,
            sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE)
        {
            led_encoder->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL)
        {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        break;
    default:
        break;
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t ws2812b_del_strip_encoder(rmt_encoder_t *encoder)
{
    ws2812b_strip_encoder_t *led_encoder = WS2812B_CONTAINEROF(encoder, ws2812b_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

RMT_ENCODER_FUNC_ATTR
static esp_err_t ws2812b_reset_strip_encoder(rmt_encoder_t *encoder)
{
    ws2812b_strip_encoder_t *led_encoder = WS2812B_CONTAINEROF(encoder, ws2812b_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t ws2812b_new_strip_encoder(uint32_t resolution_hz, rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t                 ret         = ESP_OK;
    ws2812b_strip_encoder_t  *led_encoder = NULL;

    ESP_GOTO_ON_FALSE(ret_encoder != NULL, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");

    led_encoder = rmt_alloc_encoder_mem(sizeof(ws2812b_strip_encoder_t));
    ESP_GOTO_ON_FALSE(led_encoder != NULL, ESP_ERR_NO_MEM, err, TAG, "no mem for strip encoder");

    led_encoder->base.encode = ws2812b_encode_strip;
    led_encoder->base.del    = ws2812b_del_strip_encoder;
    led_encoder->base.reset  = ws2812b_reset_strip_encoder;

    const double us_per_tick = 1000000.0 / (double)resolution_hz;

    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0    = 1,
            .duration0 = (uint32_t)(0.3 / us_per_tick),
            .level1    = 0,
            .duration1 = (uint32_t)(0.9 / us_per_tick),
        },
        .bit1 = {
            .level0    = 1,
            .duration0 = (uint32_t)(0.9 / us_per_tick),
            .level1    = 0,
            .duration1 = (uint32_t)(0.3 / us_per_tick),
        },
        .flags.msb_first = 1,
    };

    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder->bytes_encoder), err, TAG,
        "create bytes encoder failed");

    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &led_encoder->copy_encoder), err, TAG,
        "create copy encoder failed");

    const uint32_t reset_ticks = (uint32_t)((uint64_t)resolution_hz * 50ULL / 1000000ULL / 2ULL);
    led_encoder->reset_code = (rmt_symbol_word_t){
        .level0    = 0,
        .duration0 = reset_ticks,
        .level1    = 0,
        .duration1 = reset_ticks,
    };

    *ret_encoder = &led_encoder->base;
    return ESP_OK;

err:
    if (led_encoder != NULL)
    {
        if (led_encoder->bytes_encoder != NULL)
        {
            rmt_del_encoder(led_encoder->bytes_encoder);
        }
        if (led_encoder->copy_encoder != NULL)
        {
            rmt_del_encoder(led_encoder->copy_encoder);
        }
        free(led_encoder);
    }
    return ret;
}

esp_err_t ws2812b_init(ws2812b_t *dev, const ws2812b_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(dev != NULL && cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "null cfg");
    ESP_RETURN_ON_FALSE(cfg->num_leds > 0, ESP_ERR_INVALID_ARG, TAG, "num_leds");
    ESP_RETURN_ON_FALSE(cfg->gpio_num >= 0, ESP_ERR_INVALID_ARG, TAG, "gpio");

    if (dev->initialized)
    {
        ws2812b_deinit(dev);
    }

    const uint32_t res_hz = (cfg->resolution_hz != 0u) ? cfg->resolution_hz : WS2812B_DEFAULT_RESOLUTION_HZ;
    const size_t   blk    = (cfg->mem_block_symbols != 0u) ? cfg->mem_block_symbols : 64u;
    const uint8_t  qdepth = (cfg->trans_queue_depth != 0u) ? cfg->trans_queue_depth : 4u;

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = cfg->gpio_num,
        .mem_block_symbols = blk,
        .resolution_hz     = res_hz,
        .trans_queue_depth = qdepth,
    };

    rmt_channel_handle_t chan = NULL;
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &chan), TAG, "rmt_new_tx_channel");

    rmt_encoder_handle_t enc = NULL;
    esp_err_t            er  = ws2812b_new_strip_encoder(res_hz, &enc);
    if (er != ESP_OK)
    {
        rmt_del_channel(chan);
        return er;
    }

    const size_t nbuf = (size_t)cfg->num_leds * 3u;
    uint8_t     *pix  = (uint8_t *)malloc(nbuf);
    if (pix == NULL)
    {
        rmt_del_encoder(enc);
        rmt_del_channel(chan);
        return ESP_ERR_NO_MEM;
    }
    memset(pix, 0, nbuf);

    esp_err_t en = rmt_enable(chan);
    if (en != ESP_OK)
    {
        free(pix);
        rmt_del_encoder(enc);
        rmt_del_channel(chan);
        return en;
    }

    dev->chan           = chan;
    dev->encoder        = enc;
    dev->pixels         = pix;
    dev->num_leds       = cfg->num_leds;
    dev->resolution_hz  = res_hz;
    dev->initialized    = true;

    return ESP_OK;
}

void ws2812b_deinit(ws2812b_t *dev)
{
    if (dev == NULL || !dev->initialized)
    {
        return;
    }

    rmt_channel_handle_t  chan = (rmt_channel_handle_t)dev->chan;
    rmt_encoder_handle_t  enc  = (rmt_encoder_handle_t)dev->encoder;

    if (chan != NULL)
    {
        (void)rmt_disable(chan);
        (void)rmt_del_channel(chan);
    }
    if (enc != NULL)
    {
        (void)rmt_del_encoder(enc);
    }
    if (dev->pixels != NULL)
    {
        free(dev->pixels);
    }

    dev->chan        = NULL;
    dev->encoder     = NULL;
    dev->pixels      = NULL;
    dev->num_leds    = 0;
    dev->initialized = false;
}

bool ws2812b_is_initialized(const ws2812b_t *dev)
{
    return dev != NULL && dev->initialized;
}

esp_err_t ws2812b_set_pixel_rgb(ws2812b_t *dev, uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
    return ws2812b_set_pixel_grb(dev, index, g, r, b);
}

esp_err_t ws2812b_set_pixel_grb(ws2812b_t *dev, uint16_t index, uint8_t g, uint8_t r, uint8_t b)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    ESP_RETURN_ON_FALSE(index < dev->num_leds, ESP_ERR_INVALID_ARG, TAG, "index");

    uint8_t *p = dev->pixels + (size_t)index * 3u;
    p[0]        = g;
    p[1]        = r;
    p[2]        = b;
    return ESP_OK;
}

esp_err_t ws2812b_refresh(ws2812b_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");

    rmt_channel_handle_t chan = (rmt_channel_handle_t)dev->chan;
    rmt_encoder_handle_t enc  = (rmt_encoder_handle_t)dev->encoder;

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };

    const size_t nbytes = (size_t)dev->num_leds * 3u;
    ESP_RETURN_ON_ERROR(rmt_transmit(chan, enc, dev->pixels, nbytes, &tx_cfg), TAG, "rmt_transmit");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(chan, -1), TAG, "wait done");

    return ESP_OK;
}

esp_err_t ws2812b_clear(ws2812b_t *dev)
{
    ESP_RETURN_ON_FALSE(dev != NULL && dev->initialized, ESP_ERR_INVALID_STATE, TAG, "not init");
    memset(dev->pixels, 0, (size_t)dev->num_leds * 3u);
    return ws2812b_refresh(dev);
}

uint8_t *ws2812b_get_pixels(ws2812b_t *dev)
{
    if (dev == NULL || !dev->initialized)
    {
        return NULL;
    }
    return dev->pixels;
}

uint16_t ws2812b_get_num_leds(const ws2812b_t *dev)
{
    if (dev == NULL)
    {
        return 0;
    }
    return dev->num_leds;
}
