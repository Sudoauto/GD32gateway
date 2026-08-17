#ifndef HOST_SEMPHR_H
#define HOST_SEMPHR_H
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t w) {(void)s;(void)w;return pdTRUE;}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {(void)s;return pdTRUE;}
#endif
