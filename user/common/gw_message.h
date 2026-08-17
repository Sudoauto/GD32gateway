#ifndef GW_MESSAGE_H
#define GW_MESSAGE_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "gw_error.h"

#define GW_MSG_BLOCK_SIZE   320U
#define GW_MSG_BLOCK_COUNT  32U

typedef struct {
    uint16_t type;
    uint16_t length;
    uint8_t data[GW_MSG_BLOCK_SIZE];
} gw_msg_block_t;

void gw_msg_pool_init(void);
gw_msg_block_t *gw_msg_alloc(TickType_t wait_ticks);
void gw_msg_free(gw_msg_block_t *msg);
uint32_t gw_msg_pool_free_count(void);

#endif
