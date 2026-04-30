#ifndef COMMON_BUTTON_H
#define COMMON_BUTTON_H

#include "error.h"

typedef void_t (*button_cb_t)(ptr_t user_data);

common_err_t button_init(void);
common_err_t button_register_cb(button_cb_t cb, ptr_t user_data);

#endif
