#ifndef TYPE_H
#define TYPE_H

#include <stdint.h>
#include <stddef.h>

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

#endif
