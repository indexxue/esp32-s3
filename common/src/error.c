#include "error.h"

const char_t *err_to_name(common_err_t err)
{
    switch (err) {
    case COMMON_OK:
        return "COMMON_OK";
    case COMMON_ERR_INVALID_ARG:
        return "COMMON_ERR_INVALID_ARG";
    case COMMON_ERR_NOT_INIT:
        return "COMMON_ERR_NOT_INIT";
    case COMMON_ERR_INTERNAL:
        return "COMMON_ERR_INTERNAL";
    default:
        return "COMMON_ERR_UNKNOWN";
    }
}
