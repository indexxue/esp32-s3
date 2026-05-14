/**
 * @file type.h
 * @brief 固件通用宽度别名与状态类型（业务/API 层优先用本头文件命名，避免直接绑死 ESP 符号名）。
 *
 * 在 ESP-IDF 上 `status_t` 与 `ESP_*` 二进制兼容，可与 IDF API 返回值直接互传。
 */

#ifndef COMMON_TYPE_H
#define COMMON_TYPE_H

#ifndef TYPE_H
#define TYPE_H

#include <stddef.h>
#include <stdint.h>

typedef void void_t;
typedef char char_t;
typedef signed char s8_t;
typedef unsigned char u8_t;
typedef int16_t s16_t;
typedef uint16_t u16_t;
typedef int32_t s32_t;
typedef uint32_t u32_t;
typedef int64_t s64_t;
typedef uint64_t u64_t;
typedef float f32_t;
typedef double f64_t;
typedef size_t usize_t;
typedef void_t *ptr_t;
typedef u8_t bool_t;

#define FALSE ((bool_t)0U)
#define TRUE ((bool_t)1U)

#ifndef NULL_PTR
#define NULL_PTR ((void_t *)0)
#endif

#define UNUSED(param) ((void_t)(param))

#endif /* TYPE_H */

#include "esp_err.h"

/** 通用操作状态；ESP-IDF 下等价于 esp_err_t。 */
typedef esp_err_t status_t;

#define STATUS_OK ESP_OK
#define STATUS_FAIL ESP_FAIL
#define STATUS_INVALID_ARG ESP_ERR_INVALID_ARG
#define STATUS_NO_MEM ESP_ERR_NO_MEM
#define STATUS_TIMEOUT ESP_ERR_TIMEOUT
#define STATUS_NOT_SUPPORTED ESP_ERR_NOT_SUPPORTED
#define STATUS_INVALID_STATE ESP_ERR_INVALID_STATE

/** 将状态转为可读字符串（ESP-IDF 下为 esp_err_to_name）。 */
#define status_to_str(s) esp_err_to_name(s)

#endif /* COMMON_TYPE_H */
