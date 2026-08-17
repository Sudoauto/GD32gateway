#ifndef HOST_QUEUE_H
#define HOST_QUEUE_H
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
BaseType_t host_xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
#define xQueueSend(q,item,wait) host_xQueueSend((q),(item),(wait))
#endif
