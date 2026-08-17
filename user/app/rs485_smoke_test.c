#include "rs485_smoke_test.h"
#include <string.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"
#include "gateway_build_config.h"
#include "gw_log.h"
#include "gw_message.h"
#include "modbus_rtu_master.h"
#include "rs485_bus_manager.h"
#include "rtos_objects.h"

#if GW_RS485_SMOKE_TEST_ENABLE

#define SMOKE_TASK_STACK_WORDS  512U
#define SMOKE_TASK_PRIORITY     2U

static StaticTask_t s_smoke_cb;
static StackType_t s_smoke_stack[SMOKE_TASK_STACK_WORDS];
static uint32_t s_transaction_id;

static uint32_t next_transaction_id(void)
{
    ++s_transaction_id;
    if (s_transaction_id == 0U) {
        ++s_transaction_id;
    }
    return s_transaction_id;
}

static void smoke_task(void *argument)
{
    (void)argument;

    (void)xEventGroupWaitBits(g_system_events, EVT_RS485_READY,
                              pdFALSE, pdTRUE, portMAX_DELAY);
    GW_LOGI("TEST", "RS485 smoke test started: slave=%u addr=%u qty=%u",
            (unsigned)GW_RS485_TEST_SLAVE,
            (unsigned)GW_RS485_TEST_ADDRESS,
            (unsigned)GW_RS485_TEST_QUANTITY);

    for (;;) {
        if (rs485_bus_is_idle()) {
            gw_msg_block_t *request = gw_msg_alloc(pdMS_TO_TICKS(20U));
            if (request != NULL) {
                gw_err_t err = modbus_rtu_build_read_holding(
                    GW_RS485_TEST_SLAVE,
                    GW_RS485_TEST_ADDRESS,
                    GW_RS485_TEST_QUANTITY,
                    request);

                if (err != GW_OK) {
                    gw_msg_free(request);
                    GW_LOGW("TEST", "request build failed=%ld", (long)err);
                } else {
                    rs485_transaction_t txn;
                    memset(&txn, 0, sizeof(txn));
                    txn.transaction_id = next_transaction_id();
                    txn.device_id = 1U;
                    txn.protocol = RS485_PROTO_MODBUS_RTU;
                    txn.expected_rx_length = 0U;
                    txn.timeout_ms = GW_RS485_RESPONSE_TIMEOUT_MS;
                    txn.retry = GW_RS485_RETRY_COUNT;
                    txn.request = request;
                    err = rs485_bus_submit(&txn, 0U);

                    /* Ownership transfers only when submit succeeds. */
                    if (err != GW_OK) {
                        gw_msg_free(request);
                        GW_LOGW("TEST", "submit failed=%ld", (long)err);
                    }
                }
            } else {
                GW_LOGW("TEST", "message pool exhausted");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(GW_RS485_TEST_PERIOD_MS));
    }
}

void rs485_smoke_test_task_create(void)
{
    TaskHandle_t handle = xTaskCreateStatic(
        smoke_task, "mb_test", SMOKE_TASK_STACK_WORDS, NULL,
        SMOKE_TASK_PRIORITY, s_smoke_stack, &s_smoke_cb);
    configASSERT(handle != NULL);
}

#else

void rs485_smoke_test_task_create(void)
{
}

#endif
