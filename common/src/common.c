#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include "esp_log.h"

// Keep the original ESP-IDF log printer for fallback.
static vprintf_like_t s_default_log_printer = NULL;

static int common_esp_log_vprintf(const char *format, va_list args)
{
    (void)s_default_log_printer;
    return vprintf(format, args);
}

void common_init(void)
{
    s_default_log_printer = esp_log_set_vprintf(common_esp_log_vprintf);
    printf("[COMMON] init done\n");
}

void common_log(const char *tag, const char *message)
{
    const char *safe_tag = (tag != NULL) ? tag : "COMMON";
    const char *safe_message = (message != NULL) ? message : "";
    printf("[%s] %s\n", safe_tag, safe_message);
}
