/*!
    \file    rs485_test_task.c
    \brief   Standalone RS485 DMA diagnostic task (not in default build)
*/

#include "rs485_test_task.h"
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "bsp_debug_uart.h"
#include "drv_rs485.h"
#include "gateway_build_config.h"

#define TASK_STACK_WORDS    1024U
#define TASK_PRIORITY       3U
#define DIAG_PERIOD_MS      2000U

static StaticTask_t s_tcb;
static StackType_t s_stack[TASK_STACK_WORDS];
static const char k_test_msg[] = "RS485 DMA TEST\r\n";

static void clear_notifications(void)
{
    uint32_t ignored = 0U;
    (void)xTaskNotifyWait(0U, UINT32_MAX, &ignored, 0U);
}

static void rs485_dma_task(void *argument)
{
    (void)argument;

    const rs485_config_t cfg = {
        .baudrate = GW_RS485_BAUDRATE,
        .data_bits = GW_RS485_DATA_BITS,
        .stop_bits = GW_RS485_STOP_BITS,
        .parity = GW_RS485_PARITY,
    };

    if (!drv_rs485_init(&cfg) || !drv_rs485_dma_init()) {
        BSP_DEBUG_UART_WRITE_LITERAL("[RS485] diagnostic init FAILED\r\n");
        vTaskSuspend(NULL);
    }

    drv_rs485_set_task_handle(xTaskGetCurrentTaskHandle());
    drv_rs485_abort_tx();
    drv_rs485_rx_abort();
    BSP_DEBUG_UART_WRITE_LITERAL("[RS485] diagnostic ready\r\n");

    uint32_t tx_seq = 0U;
    for (;;) {
        drv_rs485_abort_tx();
        drv_rs485_rx_abort();
        clear_notifications();
        drv_rs485_set_tx();
        drv_rs485_rx_arm(0U);

        if (drv_rs485_tx_dma_start((const uint8_t *)k_test_msg,
                                   (uint16_t)(sizeof(k_test_msg) - 1U))) {
            ++tx_seq;
        }

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500U);
        bool done = false;
        while (!done && ((int32_t)(deadline - xTaskGetTickCount()) > 0)) {
            uint32_t bits = 0U;
            TickType_t wait = deadline - xTaskGetTickCount();
            (void)xTaskNotifyWait(0U, UINT32_MAX, &bits, wait);

            if ((bits & RS485_NTF_TX_DONE) != 0U) {
                char line[48];
                int n = snprintf(line, sizeof(line),
                                 "[RS485] DMA TX OK #%lu\r\n",
                                 (unsigned long)tx_seq);
                if ((n > 0) && ((size_t)n < sizeof(line))) {
                    bsp_debug_uart_write(line, (size_t)n);
                }
            }
            if ((bits & RS485_NTF_RX_EVENT) != 0U) {
                uint8_t rx[RS485_RX_DMA_BUF_SIZE];
                uint16_t rx_len = drv_rs485_rx_read(rx, sizeof(rx));
                char line[40];
                int n = snprintf(line, sizeof(line),
                                 "[RS485] RX len=%u\r\n", (unsigned)rx_len);
                if ((n > 0) && ((size_t)n < sizeof(line))) {
                    bsp_debug_uart_write(line, (size_t)n);
                }
                done = true;
            }
            if ((bits & RS485_NTF_UART_ERROR) != 0U) {
                BSP_DEBUG_UART_WRITE_LITERAL("[RS485] UART ERROR\r\n");
                done = true;
            }
        }

        drv_rs485_abort_tx();
        drv_rs485_rx_abort();
        vTaskDelay(pdMS_TO_TICKS(DIAG_PERIOD_MS));
    }
}

void rs485_test_task_create(void)
{
    TaskHandle_t h = xTaskCreateStatic(
        rs485_dma_task, "rs485dma", TASK_STACK_WORDS, NULL,
        TASK_PRIORITY, s_stack, &s_tcb);
    configASSERT(h != NULL);
}
