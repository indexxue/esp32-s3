#include "button.h"

static button_cb_t s_button_cb;
static ptr_t s_user_data;

common_err_t button_init(void)
{
    s_button_cb = 0;
    s_user_data = 0;
    return COMMON_OK;
}

common_err_t button_register_cb(button_cb_t cb, ptr_t user_data)
{
    s_button_cb = cb;
    s_user_data = user_data;
    UNUSED(s_button_cb);
    UNUSED(s_user_data);
    return COMMON_OK;
}
