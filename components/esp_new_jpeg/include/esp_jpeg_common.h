// Copyright 2025 Espressif Systems (Shanghai) CO., LTD.
// All rights reserved.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Convert four characters to FOURCC
 */
#define JPEG_FOURCC_TO_INT(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/**
 * @brief Convert 32-bit FOURCC to string
 */
static inline void jpeg_fourcc_to_str(uint32_t fourcc, char out[5])
{
    for (int i = 0; i < 4; i++) {
        out[i] = (char)((fourcc >> (i * 8)) & 0xFF);
    }
    out[4] = '\0';
}

/**
 * @brief Macro to convert an FOURCC to a string
 */
#define JPEG_FOURCC_TO_STR(fourcc) ({ \
    static char fourcc_str[5]; \
    fourcc_str[0] = fourcc_str[1] = fourcc_str[2] = fourcc_str[3] = fourcc_str[4] = '\0'; \
    jpeg_fourcc_to_str(fourcc, fourcc_str); \
    fourcc_str; \
})

typedef enum {
    JPEG_PIXEL_FORMAT_GRAY = JPEG_FOURCC_TO_INT('G', 'R', 'E', 'Y'),
    JPEG_PIXEL_FORMAT_RGB888 = JPEG_FOURCC_TO_INT('R', 'G', 'B', '3'),
    JPEG_PIXEL_FORMAT_RGBA = JPEG_FOURCC_TO_INT('R', 'A', '2', '4'),
    JPEG_PIXEL_FORMAT_YCbYCr = JPEG_FOURCC_TO_INT('Y', 'U', 'Y', 'V'),
    JPEG_PIXEL_FORMAT_YCbY2YCrY2 = JPEG_FOURCC_TO_INT('Y', 'U', 'Y', '2'),
    JPEG_PIXEL_FORMAT_RGB565_BE = JPEG_FOURCC_TO_INT('R', 'G', 'B', 'B'),
    JPEG_PIXEL_FORMAT_RGB565_LE = JPEG_FOURCC_TO_INT('R', 'G', 'B', 'L'),
    JPEG_PIXEL_FORMAT_CbYCrY = JPEG_FOURCC_TO_INT('U', 'Y', 'V', 'Y'),
} jpeg_pixel_format_t;

typedef enum {
    JPEG_ERR_OK = 0,
    JPEG_ERR_FAIL = -1,
    JPEG_ERR_NO_MEM = -2,
    JPEG_ERR_NO_MORE_DATA = -3,
    JPEG_ERR_INVALID_PARAM = -4,
    JPEG_ERR_BAD_DATA = -5,
    JPEG_ERR_UNSUPPORT_FMT = -6,
    JPEG_ERR_UNSUPPORT_STD = -7,
} jpeg_error_t;

typedef enum {
    JPEG_SUBSAMPLE_GRAY = 0,
    JPEG_SUBSAMPLE_444 = 1,
    JPEG_SUBSAMPLE_422 = 2,
    JPEG_SUBSAMPLE_420 = 3,
} jpeg_subsampling_t;

typedef enum {
    JPEG_ROTATE_0D = 0,
    JPEG_ROTATE_90D = 1,
    JPEG_ROTATE_180D = 2,
    JPEG_ROTATE_270D = 3,
} jpeg_rotate_t;

typedef struct {
    uint16_t width;
    uint16_t height;
} jpeg_resolution_t;

void *jpeg_calloc_align(size_t size, int aligned);
void jpeg_free_align(void *data);

#ifdef __cplusplus
}
#endif /* __cplusplus */
