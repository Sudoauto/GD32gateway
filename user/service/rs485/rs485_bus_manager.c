/*!
    \file    rs485_bus_manager.c
    \brief   RS485 bus manager implementation
*/

#include "rs485_bus_manager.h"
#include "rtos_objects.h"
#include "drv_rs485.h"

static volatile bool s_bus_idle = true;

gw_err_t rs485_bus_submit(const rs485_transaction_t *txn, TickType_t wait_ticks)
{
    if ((txn == NULL) || (txn->request == NULL) ||
        (txn->request->length == 0U) ||
        (txn->request->length > RS485_TX_DMA_BUF_SIZE) ||
        (txn->timeout_ms == 0U) ||
        (txn->transaction_id == 0U)) {
        return GW_ERR_PARAM;
    }
    if ((txn->protocol != RS485_PROTO_MODBUS_RTU) &&
        (txn->protocol != RS485_PROTO_RAW)) {
        return GW_ERR_PARAM;
    }
    if ((txn->expected_rx_length > RS485_RX_DMA_BUF_SIZE) ||
        (q_rs485_txn == NULL)) {
        return GW_ERR_PARAM;
    }
    if ((txn->protocol == RS485_PROTO_RAW) &&
        (txn->expected_rx_length == 0U)) {
        return GW_ERR_PARAM;
    }
    if ((txn->protocol == RS485_PROTO_MODBUS_RTU) &&
        ((txn->request->length < 4U) ||
         ((txn->expected_rx_length != 0U) &&
          (txn->expected_rx_length < 5U)))) {
        return GW_ERR_PARAM;
    }

    if (xQueueSend(q_rs485_txn, txn, wait_ticks) != pdTRUE) {
        return GW_ERR_FULL;
    }

    /* Queue send itself wakes task_rs485; a separate task-notification bit is
     * unnecessary and used to create stale-notification races. */
    return GW_OK;
}

bool rs485_bus_is_idle(void)
{
    if ((q_rs485_txn == NULL) || !s_bus_idle) {
        return false;
    }
    return uxQueueMessagesWaiting(q_rs485_txn) == 0U;
}

void rs485_bus_set_idle_internal(bool idle)
{
    s_bus_idle = idle;
}
