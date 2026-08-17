#ifndef GW_LOG_H
#define GW_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include "bsp_debug_uart.h"

/* Bring-up logger. Do not call from ISR context. */
void gw_syslog_capture(const char *level, const char *tag, const char *line, size_t length);
#if defined(__clang__) || defined(__GNUC__)
#define GW_PRINTF_LIKE(fmt_index, first_arg) \
    __attribute__((format(printf, fmt_index, first_arg)))
#else
#define GW_PRINTF_LIKE(fmt_index, first_arg)
#endif

static inline void gw_log_printf(const char *level, const char *tag,
                                 const char *fmt, ...)
    GW_PRINTF_LIKE(3, 4);

static inline void gw_log_printf(const char *level, const char *tag,
                                 const char *fmt, ...)
{
    char line[160];
    int prefix;
    int body;
    va_list ap;

    if ((level == NULL) || (tag == NULL) || (fmt == NULL)) {
        return;
    }

    prefix = snprintf(line, sizeof(line), "[%s][%s] ", level, tag);
    if ((prefix < 0) || ((size_t)prefix >= sizeof(line))) {
        return;
    }

    va_start(ap, fmt);
    body = vsnprintf(&line[prefix], sizeof(line) - (size_t)prefix, fmt, ap);
    va_end(ap);
    if (body < 0) {
        return;
    }

    size_t used = (size_t)prefix + (size_t)body;
    if (used >= sizeof(line) - 2U) {
        used = sizeof(line) - 3U;
    }
    line[used++] = '\r';
    line[used++] = '\n';
    bsp_debug_uart_write(line, used);
    gw_syslog_capture(level, tag, line, used);
}

#define GW_LOGE(tag, ...) gw_log_printf("E", (tag), __VA_ARGS__)
#define GW_LOGW(tag, ...) gw_log_printf("W", (tag), __VA_ARGS__)
#define GW_LOGI(tag, ...) gw_log_printf("I", (tag), __VA_ARGS__)
#define GW_LOGD(tag, ...) gw_log_printf("D", (tag), __VA_ARGS__)

#undef GW_PRINTF_LIKE

#endif
