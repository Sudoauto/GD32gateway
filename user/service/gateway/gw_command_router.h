#ifndef GW_COMMAND_ROUTER_H
#define GW_COMMAND_ROUTER_H

#include <stdbool.h>
#include <stdint.h>
#include "gw_error.h"
#include "modbus_rtu_master.h"

typedef enum {
    GW_COMMAND_NONE = 0,
    GW_COMMAND_CAN_SEND,
    GW_COMMAND_MODBUS_READ_HOLDING,
    GW_COMMAND_MODBUS_WRITE_SINGLE,
} gw_command_kind_t;

typedef enum {
    GW_COMMAND_IDLE = 0,
    GW_COMMAND_QUEUED,
    GW_COMMAND_OK,
    GW_COMMAND_ERROR,
} gw_command_state_t;

typedef struct {
    uint32_t sequence;
    gw_command_kind_t kind;
    gw_command_state_t state;
    gw_err_t result;
    uint64_t timestamp_ms;
    uint8_t modbus_slave;
    uint16_t modbus_register;
    uint16_t modbus_argument; /* quantity for FC03, value for FC06 */
    uint32_t can_submit_count;
    uint32_t modbus_submit_count;
    uint32_t modbus_complete_count;
    uint32_t reject_count;
} gw_command_status_t;

void gw_command_router_init(void);

gw_err_t gw_command_send_can(uint32_t can_id, bool extended, bool fd,
                             const uint8_t *data, uint8_t len);

/* Device-oriented API for scheduled/configured devices. */
gw_err_t gw_command_modbus_read_holding(uint32_t device_id,
                                         uint16_t register_address,
                                         uint16_t quantity);
gw_err_t gw_command_modbus_write_single(uint32_t device_id,
                                         uint16_t register_address,
                                         uint16_t value);

/* Operator/northbound API. The caller supplies the actual Modbus slave
 * address and does not need a pre-registered Device Manager entry. If a
 * matching configured device exists, its device_id/timeout/retry policy is
 * attached automatically. */
gw_err_t gw_command_modbus_read_holding_slave(uint8_t slave,
                                               uint16_t register_address,
                                               uint16_t quantity);
gw_err_t gw_command_modbus_write_single_slave(uint8_t slave,
                                               uint16_t register_address,
                                               uint16_t value);

bool gw_command_router_handle_modbus_result(const modbus_result_t *result);
void gw_command_router_get_status(gw_command_status_t *out);

#endif /* GW_COMMAND_ROUTER_H */
