#include "gw_command_router.h"

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "device_manager.h"
#include "drv_canfd.h"
#include "gateway_build_config.h"
#include "gw_message.h"
#include "gw_config.h"
#include "gw_log.h"
#include "gw_time.h"
#include "gw_types.h"
#include "rs485_bus_manager.h"
#include "gw_uplink.h"

#define COMMAND_CONTEXT_TAG        ((uintptr_t)0xC6000000UL)
#define COMMAND_CONTEXT_TAG_MASK   ((uintptr_t)0xFF000000UL)
#define COMMAND_CONTEXT_KIND_MASK  ((uintptr_t)0x00F00000UL)
#define COMMAND_CONTEXT_KIND_SHIFT 20U
#define COMMAND_CONTEXT_SEQ_MASK   ((uintptr_t)0x000FFFFFUL)

static gw_command_status_t s_status;
static uint32_t s_sequence;
static uint32_t s_transaction_id;

static uint32_t next_command_sequence(void)
{
    uint32_t seq;
    taskENTER_CRITICAL();
    ++s_sequence;
    if (s_sequence == 0U) ++s_sequence;
    seq = s_sequence & (uint32_t)COMMAND_CONTEXT_SEQ_MASK;
    if (seq == 0U) {
        s_sequence = 1U;
        seq = 1U;
    }
    taskEXIT_CRITICAL();
    return seq;
}

static uint32_t next_transaction_id(void)
{
    uint32_t id;
    taskENTER_CRITICAL();
    ++s_transaction_id;
    if (s_transaction_id == 0U) ++s_transaction_id;
    id = s_transaction_id;
    taskEXIT_CRITICAL();
    return id;
}

static void status_begin(gw_command_kind_t kind, uint32_t seq)
{
    taskENTER_CRITICAL();
    s_status.sequence = seq;
    s_status.kind = kind;
    s_status.state = GW_COMMAND_QUEUED;
    s_status.result = GW_OK;
    s_status.timestamp_ms = gw_time_ms();
    s_status.modbus_slave = 0U;
    s_status.modbus_register = 0U;
    s_status.modbus_argument = 0U;
    taskEXIT_CRITICAL();
}

static void status_finish(gw_command_kind_t kind, uint32_t seq, gw_err_t result)
{
    taskENTER_CRITICAL();
    /* Do not let a delayed Modbus reply overwrite the status of a newer user
     * command. The counters are still updated separately. */
    if ((s_status.sequence == seq) && (s_status.kind == kind)) {
        s_status.state = (result == GW_OK) ? GW_COMMAND_OK : GW_COMMAND_ERROR;
        s_status.result = result;
        s_status.timestamp_ms = gw_time_ms();
    }
    taskEXIT_CRITICAL();
}

void gw_command_router_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_sequence = 0U;
    s_transaction_id = 0U;
}

gw_err_t gw_command_send_can(uint32_t can_id, bool extended, bool fd,
                             const uint8_t *data, uint8_t len)
{
#if (GW_CANFD_ENABLE == 0)
    (void)can_id;
    (void)extended;
    (void)fd;
    (void)data;
    (void)len;
    return GW_ERR_NOT_SUPPORTED;
#else
    if (((data == NULL) && (len != 0U)) || (len > CANFD_MAX_DATA_BYTES) ||
        (can_id > (extended ? 0x1FFFFFFFU : 0x7FFU))) {
        taskENTER_CRITICAL();
        ++s_status.reject_count;
        taskEXIT_CRITICAL();
        return GW_ERR_PARAM;
    }

    canfd_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.id = can_id;
    frame.extended = extended;
    frame.fd = fd;
    frame.brs = false; /* Production invariant: the stable bus never uses BRS. */
    frame.len = len;
    if (len != 0U) {
        memcpy(frame.data, data, len);
    }

    uint32_t seq = next_command_sequence();
    status_begin(GW_COMMAND_CAN_SEND, seq);
    if (!drv_canfd_submit(&frame, 0U)) {
        taskENTER_CRITICAL();
        ++s_status.reject_count;
        taskEXIT_CRITICAL();
        status_finish(GW_COMMAND_CAN_SEND, seq, GW_ERR_BUSY);
        return GW_ERR_BUSY;
    }

    taskENTER_CRITICAL();
    ++s_status.can_submit_count;
    taskEXIT_CRITICAL();
    status_finish(GW_COMMAND_CAN_SEND, seq, GW_OK);
    gw_uplink_publish_can(&frame, true);
    return GW_OK;
#endif
}

static void status_set_modbus_target(uint8_t slave, uint16_t reg,
                                     uint16_t argument)
{
    taskENTER_CRITICAL();
    s_status.modbus_slave = slave;
    s_status.modbus_register = reg;
    s_status.modbus_argument = argument;
    taskEXIT_CRITICAL();
}

static gw_err_t submit_modbus_request(gw_command_kind_t kind,
                                      uint32_t device_id,
                                      uint8_t slave,
                                      uint32_t timeout_ms,
                                      uint8_t retry,
                                      uint16_t register_address,
                                      uint16_t argument,
                                      gw_msg_block_t *request,
                                      uint16_t expected_rx_length)
{
    if ((request == NULL) || (slave == 0U) || (slave > 247U) ||
        (timeout_ms == 0U)) {
        if (request != NULL) gw_msg_free(request);
        taskENTER_CRITICAL(); ++s_status.reject_count; taskEXIT_CRITICAL();
        return GW_ERR_PARAM;
    }

    uint32_t seq = next_command_sequence();
    rs485_transaction_t txn;
    memset(&txn, 0, sizeof(txn));
    txn.transaction_id = next_transaction_id();
    /* device_id=0 is valid for an operator ad-hoc transaction. The RTU slave
     * address remains in request[0], while configured devices retain their
     * stable internal ID for Point DB/device health correlation. */
    txn.device_id = device_id;
    txn.protocol = RS485_PROTO_MODBUS_RTU;
    txn.expected_rx_length = expected_rx_length;
    txn.timeout_ms = timeout_ms;
    txn.retry = retry;
    txn.request = request;
    txn.context = COMMAND_CONTEXT_TAG |
                  (((uintptr_t)kind << COMMAND_CONTEXT_KIND_SHIFT) & COMMAND_CONTEXT_KIND_MASK) |
                  ((uintptr_t)seq & COMMAND_CONTEXT_SEQ_MASK);

    status_begin(kind, seq);
    status_set_modbus_target(slave, register_address, argument);
    gw_err_t err = rs485_bus_submit(&txn, 0U);
    if (err != GW_OK) {
        gw_msg_free(request);
        taskENTER_CRITICAL(); ++s_status.reject_count; taskEXIT_CRITICAL();
        status_finish(kind, seq, err);
        return err;
    }

    taskENTER_CRITICAL(); ++s_status.modbus_submit_count; taskEXIT_CRITICAL();
    GW_LOGI("CMD", "Modbus %s queued slave=%u dev=%lu reg=%u arg=%u txn=%lu",
            (kind == GW_COMMAND_MODBUS_READ_HOLDING) ? "FC03" : "FC06",
            (unsigned)slave, (unsigned long)device_id,
            (unsigned)register_address, (unsigned)argument,
            (unsigned long)txn.transaction_id);
    return GW_OK;
}

static gw_err_t validate_configured_modbus_device(uint32_t device_id,
                                                   gw_device_t *device)
{
    if ((device == NULL) || (device_id == 0U)) return GW_ERR_PARAM;
    gw_err_t err = device_manager_get(device_id, device);
    if (err != GW_OK) return err;
    if ((device->protocol != GW_PROTO_MODBUS_RTU) ||
        (device->interface_id != GW_IF_RS485_0) ||
        (device->address == 0U) || (device->address > 247U)) {
        return GW_ERR_PARAM;
    }
    if (device->state == DEVICE_DISABLED) return GW_ERR_STATE;
    return GW_OK;
}

gw_err_t gw_command_modbus_read_holding(uint32_t device_id,
                                         uint16_t register_address,
                                         uint16_t quantity)
{
    if ((quantity == 0U) || (quantity > 125U)) return GW_ERR_PARAM;
    gw_device_t device;
    gw_err_t err = validate_configured_modbus_device(device_id, &device);
    if (err != GW_OK) {
        taskENTER_CRITICAL(); ++s_status.reject_count; taskEXIT_CRITICAL();
        return err;
    }

    gw_msg_block_t *request = gw_msg_alloc(0U);
    if (request == NULL) return GW_ERR_NO_MEMORY;
    err = modbus_rtu_build_read_holding((uint8_t)device.address,
                                        register_address, quantity, request);
    if (err != GW_OK) { gw_msg_free(request); return err; }
    return submit_modbus_request(GW_COMMAND_MODBUS_READ_HOLDING, device_id,
                                 (uint8_t)device.address,
                                 (device.timeout_ms != 0U) ? device.timeout_ms
                                                           : GW_RS485_RESPONSE_TIMEOUT_MS,
                                 device.retry, register_address, quantity,
                                 request, (uint16_t)(5U + quantity * 2U));
}

gw_err_t gw_command_modbus_write_single(uint32_t device_id,
                                         uint16_t register_address,
                                         uint16_t value)
{
    gw_device_t device;
    gw_err_t err = validate_configured_modbus_device(device_id, &device);
    if (err != GW_OK) {
        taskENTER_CRITICAL(); ++s_status.reject_count; taskEXIT_CRITICAL();
        return err;
    }

    gw_msg_block_t *request = gw_msg_alloc(0U);
    if (request == NULL) return GW_ERR_NO_MEMORY;
    err = modbus_rtu_build_write_single_register((uint8_t)device.address,
                                                  register_address, value,
                                                  request);
    if (err != GW_OK) { gw_msg_free(request); return err; }
    return submit_modbus_request(GW_COMMAND_MODBUS_WRITE_SINGLE, device_id,
                                 (uint8_t)device.address,
                                 (device.timeout_ms != 0U) ? device.timeout_ms
                                                           : GW_RS485_RESPONSE_TIMEOUT_MS,
                                 device.retry, register_address, value,
                                 request, 8U);
}

static gw_err_t resolve_slave_policy(uint8_t slave, uint32_t *device_id,
                                     uint32_t *timeout_ms, uint8_t *retry)
{
    if ((slave == 0U) || (slave > 247U) || (device_id == NULL) ||
        (timeout_ms == NULL) || (retry == NULL)) {
        return GW_ERR_PARAM;
    }

    *device_id = 0U;
    gw_runtime_config_t runtime;
    gw_config_get_runtime(&runtime);
    *timeout_ms = runtime.rs485_timeout_ms;
    *retry = runtime.rs485_retry;

    gw_device_t device;
    gw_err_t err = device_manager_find_binding(GW_PROTO_MODBUS_RTU,
                                                GW_IF_RS485_0, slave,
                                                &device);
    if (err == GW_OK) {
        if (device.state == DEVICE_DISABLED) return GW_ERR_STATE;
        *device_id = device.id;
        if (device.timeout_ms != 0U) *timeout_ms = device.timeout_ms;
        *retry = device.retry;
        return GW_OK;
    }

    /* A missing device entry is deliberately not an error for operator/manual
     * control. The RS485 transaction can still be addressed by its Modbus
     * slave ID. A transient DB mutex contention also must not prevent a manual
     * diagnostic command from reaching an otherwise idle bus. */
    if ((err == GW_ERR_NOT_FOUND) || (err == GW_ERR_BUSY)) return GW_OK;
    return err;
}

gw_err_t gw_command_modbus_read_holding_slave(uint8_t slave,
                                               uint16_t register_address,
                                               uint16_t quantity)
{
    if ((slave == 0U) || (slave > 247U) ||
        (quantity == 0U) || (quantity > 125U)) return GW_ERR_PARAM;

    uint32_t device_id, timeout_ms;
    uint8_t retry;
    gw_err_t err = resolve_slave_policy(slave, &device_id, &timeout_ms, &retry);
    if (err != GW_OK) {
        taskENTER_CRITICAL(); ++s_status.reject_count; taskEXIT_CRITICAL();
        return err;
    }

    gw_msg_block_t *request = gw_msg_alloc(0U);
    if (request == NULL) return GW_ERR_NO_MEMORY;
    err = modbus_rtu_build_read_holding(slave, register_address, quantity,
                                        request);
    if (err != GW_OK) { gw_msg_free(request); return err; }
    return submit_modbus_request(GW_COMMAND_MODBUS_READ_HOLDING, device_id,
                                 slave, timeout_ms, retry, register_address,
                                 quantity, request,
                                 (uint16_t)(5U + quantity * 2U));
}

gw_err_t gw_command_modbus_write_single_slave(uint8_t slave,
                                               uint16_t register_address,
                                               uint16_t value)
{
    if ((slave == 0U) || (slave > 247U)) return GW_ERR_PARAM;

    uint32_t device_id, timeout_ms;
    uint8_t retry;
    gw_err_t err = resolve_slave_policy(slave, &device_id, &timeout_ms, &retry);
    if (err != GW_OK) {
        taskENTER_CRITICAL(); ++s_status.reject_count; taskEXIT_CRITICAL();
        return err;
    }

    gw_msg_block_t *request = gw_msg_alloc(0U);
    if (request == NULL) return GW_ERR_NO_MEMORY;
    err = modbus_rtu_build_write_single_register(slave, register_address,
                                                  value, request);
    if (err != GW_OK) { gw_msg_free(request); return err; }
    return submit_modbus_request(GW_COMMAND_MODBUS_WRITE_SINGLE, device_id,
                                 slave, timeout_ms, retry, register_address,
                                 value, request, 8U);
}

bool gw_command_router_handle_modbus_result(const modbus_result_t *result)
{
    if ((result == NULL) ||
        ((result->context & COMMAND_CONTEXT_TAG_MASK) != COMMAND_CONTEXT_TAG)) {
        return false;
    }

    uint32_t seq = (uint32_t)(result->context & COMMAND_CONTEXT_SEQ_MASK);
    gw_command_kind_t kind = (gw_command_kind_t)
        ((result->context & COMMAND_CONTEXT_KIND_MASK) >> COMMAND_CONTEXT_KIND_SHIFT);
    if ((kind != GW_COMMAND_MODBUS_READ_HOLDING) &&
        (kind != GW_COMMAND_MODBUS_WRITE_SINGLE)) {
        kind = GW_COMMAND_MODBUS_WRITE_SINGLE;
    }
    taskENTER_CRITICAL(); ++s_status.modbus_complete_count; taskEXIT_CRITICAL();
    status_finish(kind, seq, result->result);
    return true;
}

void gw_command_router_get_status(gw_command_status_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    *out = s_status;
    taskEXIT_CRITICAL();
}
