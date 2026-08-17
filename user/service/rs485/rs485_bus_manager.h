/*!
    \file    rs485_bus_manager.h
    \brief   RS485 transaction submission service
*/

#ifndef RS485_BUS_MANAGER_H
#define RS485_BUS_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "gw_error.h"
#include "gw_message.h"

typedef enum {
    RS485_PROTO_MODBUS_RTU = 0,
    RS485_PROTO_RAW,
} rs485_protocol_t;

typedef struct {
    uint32_t            transaction_id;
    uint32_t            device_id;
    rs485_protocol_t    protocol;
    uint16_t            expected_rx_length; /* 0 = protocol auto-detect */
    uint32_t            timeout_ms;
    uint8_t             retry;
    gw_msg_block_t      *request;
    /* Opaque numeric correlation token. It is never dereferenced by the
     * RS485 stack, so it cannot become a dangling pointer. */
    uintptr_t           context;
} rs485_transaction_t;

/* On GW_OK, ownership of txn->request transfers to task_rs485. */
gw_err_t rs485_bus_submit(const rs485_transaction_t *txn, TickType_t wait_ticks);

/* True only when no transaction is active and the submission queue is empty. */
bool rs485_bus_is_idle(void);

/* Internal hook used only by task_rs485. */
void rs485_bus_set_idle_internal(bool idle);

#endif /* RS485_BUS_MANAGER_H */
