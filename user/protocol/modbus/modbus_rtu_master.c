#include "modbus_rtu_master.h"
#include <stdbool.h>
#include <string.h>

#define MODBUS_MAX_SLAVE_ADDRESS     247U
#define MODBUS_MAX_READ_BITS         2000U
#define MODBUS_MAX_READ_REGS         125U
#define MODBUS_MAX_READ_BYTES        (MODBUS_MAX_READ_REGS * 2U)
#define MODBUS_MAX_WRITE_REGS        123U
#define MODBUS_MIN_REQUEST_LEN       8U

static bool valid_slave(uint8_t slave)
{
    return (slave > 0U) && (slave <= MODBUS_MAX_SLAVE_ADDRESS);
}

static gw_err_t build_read_registers(uint8_t slave, uint8_t function,
                                     uint16_t address, uint16_t quantity,
                                     gw_msg_block_t *out)
{
    if ((out == NULL) || !valid_slave(slave) ||
        ((function != 0x03U) && (function != 0x04U)) ||
        (quantity == 0U) || (quantity > MODBUS_MAX_READ_REGS)) {
        return GW_ERR_PARAM;
    }

    out->data[0] = slave;
    out->data[1] = function;
    out->data[2] = (uint8_t)(address >> 8U);
    out->data[3] = (uint8_t)(address & 0xFFU);
    out->data[4] = (uint8_t)(quantity >> 8U);
    out->data[5] = (uint8_t)(quantity & 0xFFU);

    uint16_t crc = modbus_crc16(out->data, 6U);
    out->data[6] = (uint8_t)(crc & 0xFFU);
    out->data[7] = (uint8_t)(crc >> 8U);
    out->length = 8U;
    out->type = (uint16_t)((uint16_t)function << 8U);
    return GW_OK;
}

uint16_t modbus_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;

    if ((data == NULL) && (length != 0U)) {
        return 0U;
    }

    for (uint16_t pos = 0U; pos < length; ++pos) {
        crc ^= (uint16_t)data[pos];
        for (uint8_t i = 0U; i < 8U; ++i) {
            if ((crc & 0x0001U) != 0U) {
                crc >>= 1U;
                crc ^= 0xA001U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

gw_err_t modbus_rtu_build_read_holding(uint8_t slave, uint16_t address,
                                       uint16_t quantity, gw_msg_block_t *out)
{
    return build_read_registers(slave, 0x03U, address, quantity, out);
}

gw_err_t modbus_rtu_build_read_input(uint8_t slave, uint16_t address,
                                     uint16_t quantity, gw_msg_block_t *out)
{
    return build_read_registers(slave, 0x04U, address, quantity, out);
}

gw_err_t modbus_rtu_build_write_single_register(uint8_t slave,
                                                 uint16_t address,
                                                 uint16_t value,
                                                 gw_msg_block_t *out)
{
    if ((out == NULL) || !valid_slave(slave)) {
        return GW_ERR_PARAM;
    }

    out->data[0] = slave;
    out->data[1] = 0x06U;
    out->data[2] = (uint8_t)(address >> 8U);
    out->data[3] = (uint8_t)(address & 0xFFU);
    out->data[4] = (uint8_t)(value >> 8U);
    out->data[5] = (uint8_t)(value & 0xFFU);
    uint16_t crc = modbus_crc16(out->data, 6U);
    out->data[6] = (uint8_t)(crc & 0xFFU);
    out->data[7] = (uint8_t)(crc >> 8U);
    out->length = 8U;
    out->type = 0x0600U;
    return GW_OK;
}

gw_err_t modbus_rtu_build_write_multiple_registers(uint8_t slave,
                                                    uint16_t address,
                                                    const uint16_t *values,
                                                    uint16_t quantity,
                                                    gw_msg_block_t *out)
{
    if ((out == NULL) || (values == NULL) || !valid_slave(slave) ||
        (quantity == 0U) || (quantity > MODBUS_MAX_WRITE_REGS)) {
        return GW_ERR_PARAM;
    }

    uint16_t byte_count = (uint16_t)(quantity * 2U);
    uint16_t frame_len = (uint16_t)(9U + byte_count);
    if (frame_len > GW_MSG_BLOCK_SIZE) {
        return GW_ERR_FULL;
    }

    out->data[0] = slave;
    out->data[1] = 0x10U;
    out->data[2] = (uint8_t)(address >> 8U);
    out->data[3] = (uint8_t)(address & 0xFFU);
    out->data[4] = (uint8_t)(quantity >> 8U);
    out->data[5] = (uint8_t)(quantity & 0xFFU);
    out->data[6] = (uint8_t)byte_count;
    for (uint16_t i = 0U; i < quantity; ++i) {
        out->data[7U + (i * 2U)] = (uint8_t)(values[i] >> 8U);
        out->data[8U + (i * 2U)] = (uint8_t)(values[i] & 0xFFU);
    }

    uint16_t crc = modbus_crc16(out->data, (uint16_t)(frame_len - 2U));
    out->data[frame_len - 2U] = (uint8_t)(crc & 0xFFU);
    out->data[frame_len - 1U] = (uint8_t)(crc >> 8U);
    out->length = frame_len;
    out->type = 0x1000U;
    return GW_OK;
}

uint16_t modbus_rtu_expected_response_length(const uint8_t *frame, uint16_t length)
{
    if ((frame == NULL) || (length < 2U)) {
        return 0U;
    }

    uint8_t fc = frame[1];
    if ((fc & 0x80U) != 0U) {
        return 5U;
    }

    switch (fc) {
    case 0x01U:
    case 0x02U:
        if (length < 3U) {
            return 0U;
        }
        if (frame[2] > 250U) {
            return 0U;
        }
        return (uint16_t)(5U + frame[2]);

    case 0x03U:
    case 0x04U:
        if (length < 3U) {
            return 0U;
        }
        if ((frame[2] == 0U) || ((frame[2] & 1U) != 0U) ||
            (frame[2] > MODBUS_MAX_READ_BYTES)) {
            return 0U;
        }
        return (uint16_t)(5U + frame[2]);

    case 0x05U:
    case 0x06U:
    case 0x0FU:
    case 0x10U:
        return 8U;

    default:
        return 0U;
    }
}

gw_err_t modbus_rtu_validate_response(const uint8_t *frame, uint16_t length,
                                      uint8_t expected_slave,
                                      uint8_t expected_function,
                                      uint16_t expected_data_bytes,
                                      uint8_t *exception_code)
{
    if (exception_code != NULL) {
        *exception_code = 0U;
    }
    if ((frame == NULL) || (length < 5U) || !valid_slave(expected_slave)) {
        return GW_ERR_PARAM;
    }
    if (frame[0] != expected_slave) {
        return GW_ERR_STATE;
    }
    if ((frame[1] & 0x7FU) != expected_function) {
        return GW_ERR_STATE;
    }

    uint16_t expected_crc = (uint16_t)((uint16_t)frame[length - 2U] |
                                       ((uint16_t)frame[length - 1U] << 8U));
    uint16_t actual_crc = modbus_crc16(frame, (uint16_t)(length - 2U));
    if (expected_crc != actual_crc) {
        return GW_ERR_CRC;
    }

    if ((frame[1] & 0x80U) != 0U) {
        if (length != 5U) {
            return GW_ERR_PROTOCOL;
        }
        if (exception_code != NULL) {
            *exception_code = frame[2];
        }
        return GW_ERR_PROTOCOL;
    }

    uint16_t inferred = modbus_rtu_expected_response_length(frame, length);
    if ((inferred == 0U) || (inferred != length)) {
        return GW_ERR_PROTOCOL;
    }

    if ((expected_function == 0x01U) || (expected_function == 0x02U)) {
        if ((expected_data_bytes == 0U) || (expected_data_bytes > 250U) ||
            (frame[2] != (uint8_t)expected_data_bytes)) {
            return GW_ERR_PROTOCOL;
        }
    } else if ((expected_function == 0x03U) ||
               (expected_function == 0x04U)) {
        if ((expected_data_bytes == 0U) ||
            (expected_data_bytes > MODBUS_MAX_READ_BYTES) ||
            (frame[2] != (uint8_t)expected_data_bytes)) {
            return GW_ERR_PROTOCOL;
        }
    }

    return GW_OK;
}

gw_err_t modbus_rtu_validate_response_for_request(const uint8_t *frame,
                                                  uint16_t length,
                                                  const uint8_t *request,
                                                  uint16_t request_length,
                                                  uint8_t *exception_code)
{
    if ((request == NULL) || (request_length < MODBUS_MIN_REQUEST_LEN)) {
        return GW_ERR_PARAM;
    }

    uint8_t fc = request[1];
    uint16_t expected_data_bytes = 0U;
    if ((fc == 0x01U) || (fc == 0x02U)) {
        uint16_t qty = (uint16_t)(((uint16_t)request[4] << 8U) | request[5]);
        if ((qty == 0U) || (qty > MODBUS_MAX_READ_BITS)) {
            return GW_ERR_PARAM;
        }
        expected_data_bytes = (uint16_t)((qty + 7U) / 8U);
    } else if ((fc == 0x03U) || (fc == 0x04U)) {
        uint16_t qty = (uint16_t)(((uint16_t)request[4] << 8U) | request[5]);
        if ((qty == 0U) || (qty > MODBUS_MAX_READ_REGS)) {
            return GW_ERR_PARAM;
        }
        expected_data_bytes = (uint16_t)(qty * 2U);
    }

    gw_err_t err = modbus_rtu_validate_response(frame, length, request[0], fc,
                                                expected_data_bytes,
                                                exception_code);
    if (err != GW_OK) {
        return err;
    }

    switch (fc) {
    case 0x05U:
    case 0x06U:
        /* Successful single-write response echoes slave, function, address and
         * value exactly. CRC was already validated above. */
        if (memcmp(frame, request, 6U) != 0) {
            return GW_ERR_PROTOCOL;
        }
        break;

    case 0x0FU:
    case 0x10U:
        /* Successful multi-write response echoes slave, function, start
         * address and quantity. */
        if (memcmp(frame, request, 6U) != 0) {
            return GW_ERR_PROTOCOL;
        }
        break;

    default:
        break;
    }

    return GW_OK;
}
