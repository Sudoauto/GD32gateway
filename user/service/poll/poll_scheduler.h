#ifndef POLL_SCHEDULER_H
#define POLL_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>
#include "gw_error.h"
#include "modbus_rtu_master.h"

#define GW_MAX_POLL_JOBS 64U

typedef enum {
    POLL_ENCODING_U16 = 0,
    POLL_ENCODING_I16,
    POLL_ENCODING_U32_BE,
    POLL_ENCODING_U32_WORD_SWAP,
    POLL_ENCODING_I32_BE,
    POLL_ENCODING_I32_WORD_SWAP,
    POLL_ENCODING_F32_BE,
    POLL_ENCODING_F32_WORD_SWAP,
} poll_encoding_t;

typedef struct {
    uint32_t id;
    uint32_t device_id;
    uint32_t point_id;
    uint8_t function_code;       /* FC03 or FC04 */
    uint16_t start_address;
    uint16_t quantity;
    uint16_t register_offset;    /* offset within returned register block */
    uint32_t interval_ms;
    poll_encoding_t encoding;
    bool enabled;
} poll_job_t;

void poll_scheduler_init(void);
gw_err_t poll_scheduler_register(const poll_job_t *job);
gw_err_t poll_scheduler_upsert(const poll_job_t *job);
gw_err_t poll_scheduler_remove(uint32_t id);
void poll_scheduler_reset(void);
uint32_t poll_scheduler_snapshot(poll_job_t *out, uint32_t max_count);
gw_err_t poll_scheduler_get(uint32_t id, poll_job_t *out);
void poll_scheduler_create(void);

/* Called by task_data for every Modbus result. Returns true when the result
 * originated from the poll scheduler (success or failure). */
bool poll_scheduler_handle_modbus_result(const modbus_result_t *result);

#endif
