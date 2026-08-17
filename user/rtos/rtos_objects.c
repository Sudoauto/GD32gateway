#include "rtos_objects.h"
#include "gw_types.h"
#include "rs485_bus_manager.h"
#include "modbus_rtu_master.h"
#include "drv_canfd.h"

QueueHandle_t q_rs485_txn;
QueueHandle_t q_modbus_result;
QueueHandle_t q_point_update;
QueueHandle_t q_can_rx;
QueueHandle_t q_can_tx;

EventGroupHandle_t g_system_events;
SemaphoreHandle_t point_db_mutex;
SemaphoreHandle_t device_db_mutex;
SemaphoreHandle_t poll_db_mutex;
SemaphoreHandle_t config_db_mutex;

TaskHandle_t rs485_task_handle;
TaskHandle_t data_task_handle;
TaskHandle_t can_task_handle;

static StaticQueue_t s_q_rs485_txn_cb;
static uint8_t s_q_rs485_txn_storage[16U * sizeof(rs485_transaction_t)];
static StaticQueue_t s_q_modbus_result_cb;
static uint8_t s_q_modbus_result_storage[16U * sizeof(modbus_result_t)];
static StaticQueue_t s_q_point_update_cb;
static uint8_t s_q_point_update_storage[32U * sizeof(point_update_t)];
static StaticQueue_t s_q_can_rx_cb;
static uint8_t s_q_can_rx_storage[16U * sizeof(canfd_frame_t)];
static StaticQueue_t s_q_can_tx_cb;
static uint8_t s_q_can_tx_storage[8U * sizeof(canfd_frame_t)];

static StaticEventGroup_t s_events_cb;
static StaticSemaphore_t s_point_mutex_cb;
static StaticSemaphore_t s_device_mutex_cb;
static StaticSemaphore_t s_poll_mutex_cb;
static StaticSemaphore_t s_config_mutex_cb;

void rtos_objects_init(void)
{
    q_rs485_txn = xQueueCreateStatic(16U, sizeof(rs485_transaction_t),
                                     s_q_rs485_txn_storage, &s_q_rs485_txn_cb);
    q_modbus_result = xQueueCreateStatic(16U, sizeof(modbus_result_t),
                                         s_q_modbus_result_storage,
                                         &s_q_modbus_result_cb);
    q_point_update = xQueueCreateStatic(32U, sizeof(point_update_t),
                                        s_q_point_update_storage,
                                        &s_q_point_update_cb);
    q_can_rx = xQueueCreateStatic(16U, sizeof(canfd_frame_t),
                                  s_q_can_rx_storage, &s_q_can_rx_cb);
    q_can_tx = xQueueCreateStatic(8U, sizeof(canfd_frame_t),
                                  s_q_can_tx_storage, &s_q_can_tx_cb);

    g_system_events = xEventGroupCreateStatic(&s_events_cb);
    point_db_mutex = xSemaphoreCreateMutexStatic(&s_point_mutex_cb);
    device_db_mutex = xSemaphoreCreateMutexStatic(&s_device_mutex_cb);
    poll_db_mutex = xSemaphoreCreateMutexStatic(&s_poll_mutex_cb);
    config_db_mutex = xSemaphoreCreateMutexStatic(&s_config_mutex_cb);

    configASSERT(q_rs485_txn != NULL);
    configASSERT(q_modbus_result != NULL);
    configASSERT(q_point_update != NULL);
    configASSERT(q_can_rx != NULL);
    configASSERT(q_can_tx != NULL);
    configASSERT(g_system_events != NULL);
    configASSERT(point_db_mutex != NULL);
    configASSERT(device_db_mutex != NULL);
    configASSERT(poll_db_mutex != NULL);
    configASSERT(config_db_mutex != NULL);
}
