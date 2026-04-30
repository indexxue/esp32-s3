#ifndef COMMON_ERROR_H
#define COMMON_ERROR_H

#include "type.h"

typedef enum {
    COMMON_OK = 0,
    COMMON_ERR_INVALID_ARG = -1,
    COMMON_ERR_NOT_INIT = -2,
    COMMON_ERR_INTERNAL = -3
} common_err_t;

const char_t *err_to_name(common_err_t err);

#endif
