/**
 * @file    ws2812b.h
 * @brief   WS2812B 单总线 RGB LED 驱动（ESP-IDF RMT TX，单 GPIO 级联多灯）。
 *
 * 像素缓冲区内字节顺序为 **GRB**（与 WS2812B 数据线顺序一致）。
 * `ws2812b_set_pixel_rgb` 按常见 RGB 语义写入并自动转换为 GRB。
 */

#ifndef WS2812B_H
#define WS2812B_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 默认 RMT 分辨率 10 MHz（1 tick = 0.1 µs），与官方 led_strip 示例一致 */
#define WS2812B_DEFAULT_RESOLUTION_HZ 10000000u

typedef struct
{
    gpio_num_t          gpio_num;
    uint16_t            num_leds;
    /** RMT 分辨率（Hz）；为 0 时使用 WS2812B_DEFAULT_RESOLUTION_HZ */
    uint32_t            resolution_hz;
    /** RMT 符号块大小；为 0 时使用 64 */
    size_t              mem_block_symbols;
    /** 传输队列深度；为 0 时使用 4 */
    uint8_t             trans_queue_depth;
} ws2812b_config_t;

typedef struct
{
    void                *chan;
    void                *encoder;
    uint8_t             *pixels;
    uint16_t             num_leds;
    uint32_t             resolution_hz;
    bool                 initialized;
} ws2812b_t;

/**
 * @brief  创建 RMT 通道、WS2812 编码器并分配 GRB 像素缓冲。
 */
esp_err_t ws2812b_init(ws2812b_t *dev, const ws2812b_config_t *cfg);

/**
 * @brief  释放 RMT 与像素缓冲（可重复调用：已反初始化则直接返回）。
 */
void ws2812b_deinit(ws2812b_t *dev);

bool ws2812b_is_initialized(const ws2812b_t *dev);

/**
 * @brief  设置某一灯的 RGB（内部转换为 GRB，仅改缓冲，需调用 ws2812b_refresh 输出）。
 */
esp_err_t ws2812b_set_pixel_rgb(ws2812b_t *dev, uint16_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief  直接按数据线顺序写入 GRB（仅改缓冲）。
 */
esp_err_t ws2812b_set_pixel_grb(ws2812b_t *dev, uint16_t index, uint8_t g, uint8_t r, uint8_t b);

/**
 * @brief  将当前缓冲发送到灯带（阻塞直到本帧发送完成）。
 */
esp_err_t ws2812b_refresh(ws2812b_t *dev);

/**
 * @brief  将全部像素置零并刷新。
 */
esp_err_t ws2812b_clear(ws2812b_t *dev);

/**
 * @brief  连续 GRB 缓冲区指针（长度 3 * num_leds），用于批量填充后 ws2812b_refresh。
 */
uint8_t *ws2812b_get_pixels(ws2812b_t *dev);

uint16_t ws2812b_get_num_leds(const ws2812b_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* WS2812B_H */
