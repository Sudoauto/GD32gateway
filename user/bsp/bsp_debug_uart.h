/*!
    \file    bsp_debug_uart.h
    \brief   Debug UART BSP header for GD32H759 Gateway
*/

#ifndef BSP_DEBUG_UART_H
#define BSP_DEBUG_UART_H

#include <stddef.h>
#include <stdint.h>

void bsp_debug_uart_init(uint32_t baudrate);
void bsp_debug_uart_putc(char ch);
void bsp_debug_uart_write(const void *data, size_t length);

/* For string literals only; avoids hand-maintained byte counts. */
#define BSP_DEBUG_UART_WRITE_LITERAL(s) \
    bsp_debug_uart_write((s), sizeof(s) - 1U)

#endif /* BSP_DEBUG_UART_H */
