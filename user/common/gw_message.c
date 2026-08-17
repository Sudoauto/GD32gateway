#include "gw_message.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "queue.h"
#include "task.h"

static gw_msg_block_t s_blocks[GW_MSG_BLOCK_COUNT];
static bool s_in_use[GW_MSG_BLOCK_COUNT];
static QueueHandle_t s_free_q;
static StaticQueue_t s_free_q_cb;
static uint8_t s_free_q_storage[GW_MSG_BLOCK_COUNT * sizeof(gw_msg_block_t *)];

static bool block_index(const gw_msg_block_t *msg, uint32_t *index_out)
{
    uintptr_t base = (uintptr_t)&s_blocks[0];
    uintptr_t end = (uintptr_t)&s_blocks[GW_MSG_BLOCK_COUNT];
    uintptr_t p = (uintptr_t)msg;

    if ((msg == NULL) || (p < base) || (p >= end)) {
        return false;
    }
    uintptr_t delta = p - base;
    if ((delta % sizeof(gw_msg_block_t)) != 0U) {
        return false;
    }

    uint32_t index = (uint32_t)(delta / sizeof(gw_msg_block_t));
    if (index >= GW_MSG_BLOCK_COUNT) {
        return false;
    }
    if (index_out != NULL) {
        *index_out = index;
    }
    return true;
}

void gw_msg_pool_init(void)
{
    memset(s_blocks, 0, sizeof(s_blocks));
    memset(s_in_use, 0, sizeof(s_in_use));

    s_free_q = xQueueCreateStatic(GW_MSG_BLOCK_COUNT,
                                  sizeof(gw_msg_block_t *),
                                  s_free_q_storage,
                                  &s_free_q_cb);
    configASSERT(s_free_q != NULL);

    for (uint32_t i = 0U; i < GW_MSG_BLOCK_COUNT; ++i) {
        gw_msg_block_t *p = &s_blocks[i];
        configASSERT(xQueueSend(s_free_q, &p, 0U) == pdTRUE);
    }
}

gw_msg_block_t *gw_msg_alloc(TickType_t wait_ticks)
{
    gw_msg_block_t *p = NULL;
    uint32_t index;

    configASSERT(s_free_q != NULL);
    if (xQueueReceive(s_free_q, &p, wait_ticks) != pdTRUE) {
        return NULL;
    }

    configASSERT(block_index(p, &index));
    taskENTER_CRITICAL();
    configASSERT(!s_in_use[index]);
    s_in_use[index] = true;
    taskEXIT_CRITICAL();

    memset(p, 0, sizeof(*p));
    return p;
}

void gw_msg_free(gw_msg_block_t *msg)
{
    uint32_t index;
    bool should_enqueue = false;

    if (!block_index(msg, &index) || (s_free_q == NULL)) {
        return;
    }

    taskENTER_CRITICAL();
    if (s_in_use[index]) {
        s_in_use[index] = false;
        should_enqueue = true;
    }
    taskEXIT_CRITICAL();

    /* Ignore double-free/foreign pointers instead of corrupting the pool. */
    if (!should_enqueue) {
        return;
    }

    if (xQueueSend(s_free_q, &msg, 0U) != pdTRUE) {
        /* Queue-full here indicates internal corruption. Keep the block marked
         * in-use so it cannot be handed out twice, then assert in debug. */
        taskENTER_CRITICAL();
        s_in_use[index] = true;
        taskEXIT_CRITICAL();
        configASSERT(0);
    }
}

uint32_t gw_msg_pool_free_count(void)
{
    if (s_free_q == NULL) {
        return 0U;
    }
    return (uint32_t)uxQueueMessagesWaiting(s_free_q);
}
