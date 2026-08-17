/*!
    \file    bsp_debug_uart.c
    \brief   Debug UART BSP implementation for GD32H759 Gateway

    Hardware: USART2, TX=PC10(AF7), RX=PC11(AF7), 115200-8-N-1.
*/

#include "bsp_debug_uart.h"
#include <stdbool.h>
#include "gd32h7xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define DEBUG_USART                 USART2
#define DEBUG_USART_CLK             RCU_USART2
#define DEBUG_GPIO_CLK              RCU_GPIOC
#define DEBUG_GPIO_PORT             GPIOC
#define DEBUG_TX_PIN                GPIO_PIN_10
#define DEBUG_RX_PIN                GPIO_PIN_11
#define DEBUG_GPIO_AF               GPIO_AF_7

/* A stuck debug peripheral must never deadlock a real-time task. */
#define DEBUG_UART_TBE_SPIN_LIMIT    2000000UL

static StaticSemaphore_t s_tx_mutex_cb;
static SemaphoreHandle_t s_tx_mutex;

static bool debug_uart_putc_bounded(char ch)
{
    uint32_t guard = DEBUG_UART_TBE_SPIN_LIMIT;

    while (RESET == usart_flag_get(DEBUG_USART, USART_FLAG_TBE)) {
        if (0U == --guard) {
            return false;
        }
    }

    usart_data_transmit(DEBUG_USART, (uint8_t)ch);
    return true;
}

static void debug_uart_write_raw(const uint8_t *data, size_t length)
{
    if ((data == NULL) || (length == 0U)) {
        return;
    }

    for (size_t i = 0U; i < length; ++i) {
        if (!debug_uart_putc_bounded((char)data[i])) {
            break;
        }
    }
}

void bsp_debug_uart_init(uint32_t baudrate)
{
    rcu_periph_clock_enable(DEBUG_GPIO_CLK);
    rcu_periph_clock_enable(DEBUG_USART_CLK);

    gpio_af_set(DEBUG_GPIO_PORT, DEBUG_GPIO_AF, DEBUG_TX_PIN | DEBUG_RX_PIN);
    gpio_mode_set(DEBUG_GPIO_PORT, GPIO_MODE_AF, GPIO_PUPD_PULLUP,
                  DEBUG_TX_PIN | DEBUG_RX_PIN);
    gpio_output_options_set(DEBUG_GPIO_PORT, GPIO_OTYPE_PP,
                            GPIO_OSPEED_100_220MHZ,
                            DEBUG_TX_PIN | DEBUG_RX_PIN);

    usart_deinit(DEBUG_USART);
    usart_baudrate_set(DEBUG_USART, baudrate);
    usart_word_length_set(DEBUG_USART, USART_WL_8BIT);
    usart_stop_bit_set(DEBUG_USART, USART_STB_1BIT);
    usart_parity_config(DEBUG_USART, USART_PM_NONE);
    usart_transmit_config(DEBUG_USART, USART_TRANSMIT_ENABLE);
    usart_receive_config(DEBUG_USART, USART_RECEIVE_ENABLE);
    usart_enable(DEBUG_USART);

    s_tx_mutex = xSemaphoreCreateMutexStatic(&s_tx_mutex_cb);
    configASSERT(s_tx_mutex != NULL);
}

void bsp_debug_uart_putc(char ch)
{
    (void)debug_uart_putc_bounded(ch);
}

void bsp_debug_uart_write(const void *data, size_t length)
{
    const uint8_t *p = (const uint8_t *)data;

    if ((p == NULL) || (length == 0U)) {
        return;
    }

    /* Before the scheduler starts there is no concurrent task writer. */
#if (INCLUDE_xTaskGetSchedulerState == 1)
    if ((s_tx_mutex != NULL) &&
        (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) &&
        (__get_IPSR() == 0U)) {
        /* Never wait behind logging from another task. Dropping a debug line
         * is preferable to delaying the RS485 state machine. */
        if (xSemaphoreTake(s_tx_mutex, 0U) != pdTRUE) {
            return;
        }
        debug_uart_write_raw(p, length);
        (void)xSemaphoreGive(s_tx_mutex);
        return;
    }
#endif

    debug_uart_write_raw(p, length);
}
