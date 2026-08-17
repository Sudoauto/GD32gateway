#ifndef GW_DIAGNOSTICS_H
#define GW_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>
#include "gw_error.h"

typedef struct {
    uint64_t timestamp_ms;
    uint16_t cpu_load_permille;
    uint16_t can_load_permille;
    uint16_t rs485_load_permille;
    uint16_t rs485_loss_permille;
    uint32_t free_heap_bytes;
    uint32_t can_error_total;
    uint32_t rs485_error_total;
    uint32_t eth_rx_frames;
    uint32_t eth_tx_frames;
} gw_diag_sample_t;

typedef struct {
    bool running;
    uint32_t sample_count;
    uint16_t cpu_load_permille;
    uint16_t can_load_permille;
    uint16_t rs485_load_permille;
    uint16_t rs485_loss_permille;
    uint32_t free_heap_bytes;
} gw_diag_status_t;

typedef enum {GW_SELFTEST_IDLE=0,GW_SELFTEST_PASS,GW_SELFTEST_FAIL,GW_SELFTEST_NOT_SUPPORTED} gw_selftest_state_t;
typedef struct {
    gw_selftest_state_t can_state;
    gw_selftest_state_t rs485_state;
    gw_selftest_state_t can_fixture_state;
    gw_selftest_state_t rs485_fixture_state;
    uint64_t timestamp_ms;
    gw_err_t can_result;
    gw_err_t rs485_result;
    gw_err_t can_fixture_result;
    gw_err_t rs485_fixture_result;
} gw_selftest_status_t;

void gw_diagnostics_init(void);
void gw_diagnostics_task_create(void);
void gw_diagnostics_get_status(gw_diag_status_t *out);
uint32_t gw_diagnostics_history(gw_diag_sample_t *out,uint32_t max_count);
void gw_diagnostics_run_selftest(void);
void gw_diagnostics_get_selftest(gw_selftest_status_t *out);
/* Optional board/fixture hooks for an active physical-layer loopback test.
 * Default implementations return GW_ERR_NOT_SUPPORTED. Do not electrically
 * short CAN_H to CAN_L or RS485_A to RS485_B; use a proper loopback fixture
 * or a second transceiver/test node. */
gw_err_t gw_diag_fixture_can_loopback(void);
gw_err_t gw_diag_fixture_rs485_loopback(void);

/* Compact read-only metric IDs consumed by SNMP/HMI. */
uint32_t gw_diagnostics_metric_u32(uint16_t metric_id);

#endif
