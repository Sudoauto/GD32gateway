#include "task_data.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "device_manager.h"
#include "gw_log.h"
#include "gw_message.h"
#include "gw_time.h"
#include "gw_types.h"
#include "gw_command_router.h"
#include "gw_alarm_rules.h"
#include "gw_uplink.h"
#include "gw_watchdog.h"
#include "modbus_rtu_master.h"
#include "point_db.h"
#include "poll_scheduler.h"
#include "rtos_objects.h"

#define DATA_TASK_STACK_WORDS  1024U
#define DATA_TASK_PRIORITY     4U

static StaticTask_t s_task_cb;
static StackType_t s_stack[DATA_TASK_STACK_WORDS];

static void dump_fc03(const modbus_result_t *r)
{
    if ((r == NULL) || (r->payload == NULL) || (r->payload_length < 5U)) {
        return;
    }

    const uint8_t *f = r->payload->data;
    uint8_t byte_count = f[2];
    if ((f[1] != 0x03U) || (byte_count == 0U) ||
        ((byte_count & 1U) != 0U) ||
        (r->payload_length != (uint16_t)(5U + byte_count))) {
        return;
    }

    uint16_t reg_count = (uint16_t)(byte_count / 2U);
    GW_LOGI("MODBUS", "slave=%u dev=%lu FC03 regs=%u",
            (unsigned)r->slave_address, (unsigned long)r->device_id,
            (unsigned)reg_count);

    for (uint16_t i = 0U; i < reg_count; ++i) {
        uint16_t raw = (uint16_t)(((uint16_t)f[3U + i * 2U] << 8U) |
                                  (uint16_t)f[4U + i * 2U]);
        GW_LOGI("MODBUS", "  reg[%u]=%u (0x%04X)",
                (unsigned)i, (unsigned)raw, (unsigned)raw);
    }
}

static void dump_fc06(const modbus_result_t *r)
{
    if ((r == NULL) || (r->payload == NULL) || (r->payload_length != 8U)) {
        return;
    }
    const uint8_t *f = r->payload->data;
    if (f[1] != 0x06U) return;
    uint16_t reg = (uint16_t)(((uint16_t)f[2] << 8U) | f[3]);
    uint16_t value = (uint16_t)(((uint16_t)f[4] << 8U) | f[5]);
    GW_LOGI("MODBUS", "slave=%u dev=%lu FC06 reg=%u value=%u (0x%04X)",
            (unsigned)r->slave_address, (unsigned long)r->device_id,
            (unsigned)reg, (unsigned)value, (unsigned)value);
}

static void handle_modbus_result(modbus_result_t *r)
{
    if (r == NULL) {
        return;
    }

    uint64_t now_ms = gw_time_ms();
    if (r->result == GW_OK) {
        if (r->device_id != 0U) {
            device_manager_report_success(r->device_id, now_ms);
        }
        dump_fc03(r);
        dump_fc06(r);
    } else {
        /* A Modbus exception proves the slave is alive; it is an application/
         * register-level failure, not a link outage. */
        if ((r->result == GW_ERR_PROTOCOL) && (r->exception_code != 0U)) {
            if (r->device_id != 0U) {
                device_manager_report_success(r->device_id, now_ms);
            }
        } else if (r->device_id != 0U) {
            device_manager_report_failure(r->device_id, r->result, now_ms);
        }
        if (r->result == GW_ERR_TIMEOUT) {
            GW_LOGW("MODBUS", "slave=%u dev=%lu timeout",
                    (unsigned)r->slave_address, (unsigned long)r->device_id);
        } else if (r->result == GW_ERR_CRC) {
            GW_LOGW("MODBUS", "slave=%u dev=%lu CRC error",
                    (unsigned)r->slave_address, (unsigned long)r->device_id);
        } else if ((r->result == GW_ERR_PROTOCOL) &&
                   (r->exception_code != 0U)) {
            GW_LOGW("MODBUS", "slave=%u dev=%lu exception=0x%02X",
                    (unsigned)r->slave_address, (unsigned long)r->device_id,
                    (unsigned)r->exception_code);
        } else {
            GW_LOGW("MODBUS", "slave=%u dev=%lu error=%ld",
                    (unsigned)r->slave_address, (unsigned long)r->device_id,
                    (long)r->result);
        }
    }

    (void)gw_command_router_handle_modbus_result(r);
    (void)poll_scheduler_handle_modbus_result(r);

    if (r->payload != NULL) {
        gw_msg_free(r->payload);
        r->payload = NULL;
    }
}

static void data_task(void *argument)
{
    (void)argument;
    point_update_t update;
    modbus_result_t result;

    for (;;) {
        gw_watchdog_beat(GW_WD_DATA);
        if (xQueueReceive(q_point_update, &update,
                          pdMS_TO_TICKS(20U)) == pdTRUE) {
            gw_err_t update_err = point_db_update(&update);
            if (update_err == GW_ERR_BUSY) {
                /* Do not silently lose a decoded field update because another
                 * short Point DB operation briefly owned the mutex. */
                vTaskDelay(pdMS_TO_TICKS(1U));
                update_err = point_db_update(&update);
            }
            if (update_err != GW_OK) {
                GW_LOGW("DATA", "point=%lu update dropped err=%ld",
                        (unsigned long)update.point_id, (long)update_err);
            } else {
                /* Every protocol converges here after committing to Point DB.
                 * Alarm/rule evaluation and offline recording therefore have
                 * identical semantics for CAN and Modbus RTU. */
                gw_automation_on_point(update.point_id);
                gw_uplink_publish_point_record(update.point_id);
            }
        }

        while (xQueueReceive(q_modbus_result, &result, 0U) == pdTRUE) {
            handle_modbus_result(&result);
        }
    }
}

void task_data_create(void)
{
    data_task_handle = xTaskCreateStatic(
        data_task, "data", DATA_TASK_STACK_WORDS, NULL,
        DATA_TASK_PRIORITY, s_stack, &s_task_cb);
    configASSERT(data_task_handle != NULL);
}
