#ifndef HOST_RTOS_OBJECTS_H
#define HOST_RTOS_OBJECTS_H
#include "queue.h"
#include "semphr.h"
extern QueueHandle_t q_point_update;
extern SemaphoreHandle_t point_db_mutex;
extern SemaphoreHandle_t device_db_mutex;
extern SemaphoreHandle_t poll_db_mutex;
extern SemaphoreHandle_t config_db_mutex;
#endif
