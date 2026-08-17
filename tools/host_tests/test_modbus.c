#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "modbus_rtu_master.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

static uint16_t append_crc(uint8_t *p, uint16_t n)
{
    uint16_t crc = modbus_crc16(p, n);
    p[n] = (uint8_t)crc;
    p[n + 1U] = (uint8_t)(crc >> 8U);
    return (uint16_t)(n + 2U);
}

int main(void)
{
    static const uint8_t crc_vector[] = "123456789";
    CHECK(modbus_crc16(crc_vector, 9U) == 0x4B37U);

    gw_msg_block_t req;
    memset(&req, 0, sizeof(req));
    CHECK(modbus_rtu_build_read_holding(1U, 0x0010U, 2U, &req) == GW_OK);
    CHECK(req.length == 8U && req.data[1] == 0x03U);

    uint8_t fc03[9] = {1U, 0x03U, 4U, 0x12U, 0x34U, 0x56U, 0x78U};
    uint16_t fc03_len = append_crc(fc03, 7U);
    CHECK(modbus_rtu_validate_response_for_request(fc03, fc03_len,
                                                    req.data, req.length,
                                                    NULL) == GW_OK);
    fc03[2] = 2U;
    fc03_len = append_crc(fc03, 5U);
    CHECK(modbus_rtu_validate_response_for_request(fc03, fc03_len,
                                                    req.data, req.length,
                                                    NULL) == GW_ERR_PROTOCOL);

    /* FC01: 9 coils require exactly 2 response data bytes. */
    uint8_t fc01_req[8] = {1U, 0x01U, 0U, 0U, 0U, 9U, 0U, 0U};
    append_crc(fc01_req, 6U);
    uint8_t fc01_ok[7] = {1U, 0x01U, 2U, 0x55U, 0x01U};
    uint16_t fc01_ok_len = append_crc(fc01_ok, 5U);
    CHECK(modbus_rtu_validate_response_for_request(fc01_ok, fc01_ok_len,
                                                    fc01_req, sizeof(fc01_req),
                                                    NULL) == GW_OK);
    uint8_t fc01_bad[8] = {1U, 0x01U, 3U, 0x55U, 0x01U, 0U};
    uint16_t fc01_bad_len = append_crc(fc01_bad, 6U);
    CHECK(modbus_rtu_validate_response_for_request(fc01_bad, fc01_bad_len,
                                                    fc01_req, sizeof(fc01_req),
                                                    NULL) == GW_ERR_PROTOCOL);

    /* Write-single response must echo address and value. */
    memset(&req, 0, sizeof(req));
    CHECK(modbus_rtu_build_write_single_register(2U, 0x1234U, 0xABCDU, &req) == GW_OK);
    CHECK(modbus_rtu_validate_response_for_request(req.data, req.length,
                                                    req.data, req.length,
                                                    NULL) == GW_OK);
    uint8_t echo_bad[8];
    memcpy(echo_bad, req.data, sizeof(echo_bad));
    echo_bad[5] ^= 1U;
    append_crc(echo_bad, 6U);
    CHECK(modbus_rtu_validate_response_for_request(echo_bad, sizeof(echo_bad),
                                                    req.data, req.length,
                                                    NULL) == GW_ERR_PROTOCOL);

    /* Exception frame is protocol failure but exposes the exception code. */
    uint8_t ex[5] = {2U, 0x86U, 0x02U};
    append_crc(ex, 3U);
    uint8_t exception_code = 0U;
    CHECK(modbus_rtu_validate_response_for_request(ex, sizeof(ex),
                                                    req.data, req.length,
                                                    &exception_code) == GW_ERR_PROTOCOL);
    CHECK(exception_code == 0x02U);

    if (failures != 0) return 1;
    puts("modbus regression: PASS");
    return 0;
}
