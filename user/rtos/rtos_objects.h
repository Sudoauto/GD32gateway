#ifndef RTOS_OBJECTS_H
#define RTOS_OBJECTS_H

#include "FreeRTOS.h"
#include "queue.h"
#include "event_groups.h"
#include "semphr.h"
#include "task.h"

#define EVT_CONFIG_READY      (1UL << 0)
#define EVT_ETH_LINK_UP       (1UL << 1)
#define EVT_NET_IP_READY      (1UL << 2)
#define EVT_TCP_SERVER_READY   (1UL << 3)
#define EVT_TCP_CLIENT_CONNECTED (1UL << 4)
#define EVT_UPLINK_SERVER_READY  (1UL << 5)
#define EVT_RS485_READY       (1UL << 6)
#define EVT_CANFD_READY       (1UL << 7)
#define EVT_UPLINK_CLIENT_CONNECTED (1UL << 8)
#define EVT_SYSTEM_RUNNING    (1UL << 9)
#define EVT_SYSTEM_DEGRADED   (1UL << 10)
#define EVT_CONFIG_UPDATING    (1UL << 11)
#define EVT_TIME_SYNCED        (1UL << 12)
#define EVT_AUTH_UNLOCKED      (1UL << 13)
#define EVT_FACTORY_RESET_REQ  (1UL << 14)

extern QueueHandle_t q_rs485_txn;
extern QueueHandle_t q_modbus_result;
extern QueueHandle_t q_point_update;
extern QueueHandle_t q_can_rx;
extern QueueHandle_t q_can_tx;

extern EventGroupHandle_t g_system_events;
extern SemaphoreHandle_t point_db_mutex;
extern SemaphoreHandle_t device_db_mutex;
extern SemaphoreHandle_t poll_db_mutex;
extern SemaphoreHandle_t config_db_mutex;

extern TaskHandle_t rs485_task_handle;
extern TaskHandle_t data_task_handle;
extern TaskHandle_t can_task_handle;

void rtos_objects_init(void);

#endif
