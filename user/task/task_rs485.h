#ifndef TASK_RS485_H
#define TASK_RS485_H

#include <stdint.h>

typedef struct {
    uint32_t attempt_count;
    uint32_t retry_count;
    uint32_t final_ok_count;
    uint32_t final_timeout_count;
    uint32_t final_crc_count;
    uint32_t final_protocol_count;
    uint32_t final_io_count;
} rs485_task_stats_t;

void task_rs485_create(void);
void task_rs485_get_stats(rs485_task_stats_t *out);

#endif
