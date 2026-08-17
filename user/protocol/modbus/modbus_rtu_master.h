#ifndef MODBUS_RTU_MASTER_H
#define MODBUS_RTU_MASTER_H

#include <stdint.h>
#include "gw_error.h"
#include "gw_message.h"

typedef struct {
    uint32_t transaction_id;
    uint32_t device_id;
    gw_err_t result;
    uint8_t slave_address;
    uint8_t function_code;
    uint8_t exception_code;
    uint16_t payload_length;
    gw_msg_block_t *payload;
    uintptr_t context;
} modbus_result_t;

uint16_t modbus_crc16(const uint8_t *data, uint16_t length);

gw_err_t modbus_rtu_build_read_holding(uint8_t slave, uint16_t address,
                                       uint16_t quantity, gw_msg_block_t *out);
gw_err_t modbus_rtu_build_read_input(uint8_t slave, uint16_t address,
                                     uint16_t quantity, gw_msg_block_t *out);
gw_err_t modbus_rtu_build_write_single_register(uint8_t slave,
                                                 uint16_t address,
                                                 uint16_t value,
                                                 gw_msg_block_t *out);
gw_err_t modbus_rtu_build_write_multiple_registers(uint8_t slave,
                                                    uint16_t address,
                                                    const uint16_t *values,
                                                    uint16_t quantity,
                                                    gw_msg_block_t *out);

/* Return 0 while too few bytes are available to infer a legal frame length. */
uint16_t modbus_rtu_expected_response_length(const uint8_t *frame, uint16_t length);

/* Legacy field-based validator retained for callers that already know the
 * expected function/data size. New transaction code should use the request-
 * aware validator below so write echoes are checked too. */
gw_err_t modbus_rtu_validate_response(const uint8_t *frame, uint16_t length,
                                      uint8_t expected_slave,
                                      uint8_t expected_function,
                                      uint16_t expected_data_bytes,
                                      uint8_t *exception_code);

/* Validate a response against the original RTU request, including CRC,
 * slave/function matching, read byte-count, and FC05/06/0F/10 echo fields. */
gw_err_t modbus_rtu_validate_response_for_request(const uint8_t *frame,
                                                  uint16_t length,
                                                  const uint8_t *request,
                                                  uint16_t request_length,
                                                  uint8_t *exception_code);

#endif
