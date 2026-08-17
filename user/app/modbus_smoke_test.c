/*!
    \file    modbus_smoke_test.c
    \brief   Standalone Modbus smoke test (not used by the default app)

    Kept as a diagnostic alternative. The default gateway_app uses
    rs485_smoke_test + task_data so q_modbus_result has a single consumer.
*/

#include "modbus_smoke_test.h"
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "bsp_debug_uart.h"
#include "gateway_build_config.h"
#include "gw_message.h"
#include "modbus_rtu_master.h"
#include "rs485_bus_manager.h"
#include "rtos_objects.h"

#define SMOKE_STACK_WORDS   512U
#define SMOKE_PRIORITY      2U

static StaticTask_t s_tcb;
static StackType_t s_stack[SMOKE_STACK_WORDS];
static uint32_t s_txn_id;

static uint32_t next_txn_id(void)
{
    ++s_txn_id;
    if (s_txn_id == 0U) ++s_txn_id;
    return s_txn_id;
}

static bool wait_for_result(uint32_t transaction_id, modbus_result_t *out)
{
    TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS((GW_RS485_RETRY_COUNT + 1U) *
                      GW_RS485_RESPONSE_TIMEOUT_MS + 500U);

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = ((int32_t)(deadline - now) > 0)
                          ? (deadline - now) : 0U;
        modbus_result_t r;
        if (xQueueReceive(q_modbus_result, &r, wait) != pdTRUE) {
            return false;
        }

        if (r.transaction_id == transaction_id) {
            *out = r;
            return true;
        }

        /* A late result from an older standalone transaction must never be
         * mistaken for the current request. */
        if (r.payload != NULL) {
            gw_msg_free(r.payload);
        }

        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return false;
        }
    }
}

static void smoke_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(50U));
    BSP_DEBUG_UART_WRITE_LITERAL("[MB] Modbus poll started\r\n");

    for (;;) {
        gw_msg_block_t *req = gw_msg_alloc(pdMS_TO_TICKS(100U));
        if (req == NULL) {
            BSP_DEBUG_UART_WRITE_LITERAL("[MB] alloc FAILED\r\n");
            vTaskDelay(pdMS_TO_TICKS(GW_RS485_TEST_PERIOD_MS));
            continue;
        }

        gw_err_t err = modbus_rtu_build_read_holding(
            GW_RS485_TEST_SLAVE, GW_RS485_TEST_ADDRESS,
            GW_RS485_TEST_QUANTITY, req);
        if (err != GW_OK) {
            BSP_DEBUG_UART_WRITE_LITERAL("[MB] build FAILED\r\n");
            gw_msg_free(req);
            vTaskDelay(pdMS_TO_TICKS(GW_RS485_TEST_PERIOD_MS));
            continue;
        }

        rs485_transaction_t txn;
        memset(&txn, 0, sizeof(txn));
        txn.transaction_id = next_txn_id();
        txn.device_id = 1U;
        txn.protocol = RS485_PROTO_MODBUS_RTU;
        txn.timeout_ms = GW_RS485_RESPONSE_TIMEOUT_MS;
        txn.retry = GW_RS485_RETRY_COUNT;
        txn.request = req;

        err = rs485_bus_submit(&txn, pdMS_TO_TICKS(100U));
        if (err != GW_OK) {
            BSP_DEBUG_UART_WRITE_LITERAL("[MB] submit FAILED\r\n");
            gw_msg_free(req);
            vTaskDelay(pdMS_TO_TICKS(GW_RS485_TEST_PERIOD_MS));
            continue;
        }

        modbus_result_t res;
        memset(&res, 0, sizeof(res));
        if (!wait_for_result(txn.transaction_id, &res)) {
            BSP_DEBUG_UART_WRITE_LITERAL("[MB] result timeout\r\n");
        } else if (res.result == GW_OK) {
            char line[64];
            int n = snprintf(line, sizeof(line), "[MB] OK txn=%lu\r\n",
                             (unsigned long)res.transaction_id);
            if ((n > 0) && ((size_t)n < sizeof(line))) {
                bsp_debug_uart_write(line, (size_t)n);
            }
        } else if (res.result == GW_ERR_TIMEOUT) {
            BSP_DEBUG_UART_WRITE_LITERAL("[MB] TIMEOUT\r\n");
        } else if (res.result == GW_ERR_CRC) {
            BSP_DEBUG_UART_WRITE_LITERAL("[MB] CRC ERROR\r\n");
        } else {
            char line[64];
            int n = snprintf(line, sizeof(line), "[MB] ERROR code=%ld\r\n",
                             (long)res.result);
            if ((n > 0) && ((size_t)n < sizeof(line))) {
                bsp_debug_uart_write(line, (size_t)n);
            }
        }

        if (res.payload != NULL) {
            gw_msg_free(res.payload);
        }
        vTaskDelay(pdMS_TO_TICKS(GW_RS485_TEST_PERIOD_MS));
    }
}

void modbus_smoke_test_create(void)
{
    TaskHandle_t handle = xTaskCreateStatic(
        smoke_task, "mbsmoke", SMOKE_STACK_WORDS, NULL,
        SMOKE_PRIORITY, s_stack, &s_tcb);
    configASSERT(handle != NULL);
}
