#include "poll_scheduler.h"
#include <float.h>
#include <string.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "semphr.h"
#include "task.h"
#include "device_manager.h"
#include "gw_log.h"
#include "gw_message.h"
#include "gw_time.h"
#include "gw_types.h"
#include "gw_watchdog.h"
#include "point_db.h"
#include "rs485_bus_manager.h"
#include "rtos_objects.h"

#define POLL_TASK_STACK_WORDS      768U
#define POLL_TASK_PRIORITY         3U
#define POLL_TASK_PERIOD_MS        10U
#define POLL_SUBMIT_RETRY_MS       50U
#define POLL_CONTEXT_TAG           ((uintptr_t)0xA5000000UL)
#define POLL_CONTEXT_TAG_MASK      ((uintptr_t)0xFF000000UL)
#define POLL_CONTEXT_INDEX_MASK    ((uintptr_t)0x0000FFFFUL)

static StaticTask_t s_poll_task_cb;
static StackType_t s_poll_stack[POLL_TASK_STACK_WORDS];
static poll_job_t s_jobs[GW_MAX_POLL_JOBS];
static uint64_t s_next_due_ms[GW_MAX_POLL_JOBS];
static uint32_t s_job_count;
static uint32_t s_transaction_id;

static int32_t find_job_index(uint32_t id)
{
    for (uint32_t i = 0U; i < s_job_count; ++i) {
        if (s_jobs[i].id == id) {
            return (int32_t)i;
        }
    }
    return -1;
}

static uint16_t encoding_register_width(poll_encoding_t encoding)
{
    switch (encoding) {
    case POLL_ENCODING_U16:
    case POLL_ENCODING_I16:
        return 1U;
    case POLL_ENCODING_U32_BE:
    case POLL_ENCODING_U32_WORD_SWAP:
    case POLL_ENCODING_I32_BE:
    case POLL_ENCODING_I32_WORD_SWAP:
    case POLL_ENCODING_F32_BE:
    case POLL_ENCODING_F32_WORD_SWAP:
        return 2U;
    default:
        return 0U;
    }
}

static uint32_t next_transaction_id(void)
{
    ++s_transaction_id;
    if (s_transaction_id == 0U) {
        ++s_transaction_id;
    }
    return s_transaction_id;
}

static uintptr_t make_context(uint32_t index)
{
    return POLL_CONTEXT_TAG | (uintptr_t)(index + 1U);
}

static bool context_to_index(uintptr_t context, uint32_t *index_out)
{
    if (((context & POLL_CONTEXT_TAG_MASK) != POLL_CONTEXT_TAG) ||
        (index_out == NULL)) {
        return false;
    }
    uintptr_t encoded = context & POLL_CONTEXT_INDEX_MASK;
    if ((encoded == 0U) || (encoded > GW_MAX_POLL_JOBS)) {
        return false;
    }
    *index_out = (uint32_t)(encoded - 1U);
    return true;
}

static gw_err_t build_request(const poll_job_t *job, uint8_t slave,
                              gw_msg_block_t *request)
{
    if (job->function_code == 0x03U) {
        return modbus_rtu_build_read_holding(slave, job->start_address,
                                             job->quantity, request);
    }
    if (job->function_code == 0x04U) {
        return modbus_rtu_build_read_input(slave, job->start_address,
                                           job->quantity, request);
    }
    return GW_ERR_NOT_SUPPORTED;
}

static bool copy_job(uint32_t index, poll_job_t *job, uint64_t *next_due)
{
    if ((job == NULL) || (next_due == NULL) || (poll_db_mutex == NULL)) {
        return false;
    }
    if (xSemaphoreTake(poll_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return false;
    }
    bool ok = index < s_job_count;
    if (ok) {
        *job = s_jobs[index];
        *next_due = s_next_due_ms[index];
    }
    (void)xSemaphoreGive(poll_db_mutex);
    return ok;
}

static void set_next_due(uint32_t index, uint64_t next_due)
{
    if (poll_db_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(poll_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return;
    }
    if (index < s_job_count) {
        s_next_due_ms[index] = next_due;
    }
    (void)xSemaphoreGive(poll_db_mutex);
}

static bool decode_raw_value(const poll_job_t *job, const uint8_t *registers,
                             uint16_t register_bytes, double *out)
{
    if ((job == NULL) || (registers == NULL) || (out == NULL)) {
        return false;
    }

    uint16_t width = encoding_register_width(job->encoding);
    uint32_t byte_offset = (uint32_t)job->register_offset * 2U;
    uint32_t bytes_needed = (uint32_t)width * 2U;
    if ((width == 0U) || ((byte_offset + bytes_needed) > register_bytes)) {
        return false;
    }

    const uint8_t *p = &registers[byte_offset];
    uint16_t w0 = (uint16_t)(((uint16_t)p[0] << 8U) | p[1]);

    switch (job->encoding) {
    case POLL_ENCODING_U16:
        *out = (double)w0;
        return true;
    case POLL_ENCODING_I16:
        *out = (double)(int16_t)w0;
        return true;
    case POLL_ENCODING_U32_BE:
    case POLL_ENCODING_U32_WORD_SWAP:
    case POLL_ENCODING_I32_BE:
    case POLL_ENCODING_I32_WORD_SWAP:
    case POLL_ENCODING_F32_BE:
    case POLL_ENCODING_F32_WORD_SWAP:
        break;
    default:
        return false;
    }

    uint16_t w1 = (uint16_t)(((uint16_t)p[2] << 8U) | p[3]);
    uint32_t u32;
    if ((job->encoding == POLL_ENCODING_U32_WORD_SWAP) ||
        (job->encoding == POLL_ENCODING_I32_WORD_SWAP) ||
        (job->encoding == POLL_ENCODING_F32_WORD_SWAP)) {
        u32 = ((uint32_t)w1 << 16U) | w0;
    } else {
        u32 = ((uint32_t)w0 << 16U) | w1;
    }

    switch (job->encoding) {
    case POLL_ENCODING_U16:
    case POLL_ENCODING_I16:
        return false;
    case POLL_ENCODING_U32_BE:
    case POLL_ENCODING_U32_WORD_SWAP:
        *out = (double)u32;
        return true;
    case POLL_ENCODING_I32_BE:
    case POLL_ENCODING_I32_WORD_SWAP:
        *out = (double)(int32_t)u32;
        return true;
    case POLL_ENCODING_F32_BE:
    case POLL_ENCODING_F32_WORD_SWAP: {
        float f;
        memcpy(&f, &u32, sizeof(f));
        *out = (double)f;
        return true;
    }
    default:
        return false;
    }
}

static bool value_from_double(gw_value_type_t type, double value,
                              gw_value_t *out)
{
    if ((out == NULL) || (value != value) ||
        (value > DBL_MAX) || (value < -DBL_MAX)) {
        return false;
    }

    switch (type) {
    case GW_VALUE_BOOL:
        out->b = (value != 0.0);
        return true;
    case GW_VALUE_U16:
        if ((value < 0.0) || (value > 65535.0)) return false;
        out->u16 = (uint16_t)value;
        return true;
    case GW_VALUE_I16:
        if ((value < -32768.0) || (value > 32767.0)) return false;
        out->i16 = (int16_t)value;
        return true;
    case GW_VALUE_U32:
        if ((value < 0.0) || (value > 4294967295.0)) return false;
        out->u32 = (uint32_t)value;
        return true;
    case GW_VALUE_I32:
        if ((value < -2147483648.0) || (value > 2147483647.0)) return false;
        out->i32 = (int32_t)value;
        return true;
    case GW_VALUE_F32:
        if ((value < -(double)FLT_MAX) || (value > (double)FLT_MAX)) return false;
        out->f32 = (float)value;
        return true;
    case GW_VALUE_F64:
        out->f64 = value;
        return true;
    default:
        return false;
    }
}

static void mark_job_failure(const poll_job_t *job, gw_err_t result)
{
    if (job == NULL) {
        return;
    }

    /* task_data reports the transport result to Device Manager before calling
     * this hook. Preserve the stronger OFFLINE device-level quality on the
     * third consecutive link failure instead of overwriting it with TIMEOUT. */
    gw_quality_t quality = (result == GW_ERR_TIMEOUT) ?
                           GW_QUALITY_TIMEOUT : GW_QUALITY_BAD;
    gw_device_t device;
    if ((device_manager_get(job->device_id, &device) == GW_OK) &&
        (device.state == DEVICE_OFFLINE)) {
        quality = GW_QUALITY_OFFLINE;
    }
    (void)point_db_set_quality(job->point_id, quality, gw_time_data_ms());
}

static gw_err_t apply_result(const poll_job_t *job, const modbus_result_t *result)
{
    if ((job == NULL) || (result == NULL) || (result->payload == NULL) ||
        (result->payload_length < 5U)) {
        return GW_ERR_PARAM;
    }

    const uint8_t *frame = result->payload->data;
    if ((frame[1] != job->function_code) ||
        (frame[2] != (uint8_t)(job->quantity * 2U)) ||
        (result->payload_length != (uint16_t)(5U + frame[2]))) {
        return GW_ERR_PROTOCOL;
    }

    gw_point_t point;
    gw_err_t err = point_db_get(job->point_id, &point);
    if (err != GW_OK) {
        return err;
    }
    if (point.device_id != job->device_id) {
        return GW_ERR_STATE;
    }

    double raw;
    if (!decode_raw_value(job, &frame[3], frame[2], &raw)) {
        return GW_ERR_PROTOCOL;
    }
    double scaled = (raw * (double)point.scale) + (double)point.offset;

    point_update_t update;
    memset(&update, 0, sizeof(update));
    update.point_id = point.id;
    update.device_id = point.device_id;
    update.type = point.type;
    if (!value_from_double(point.type, scaled, &update.value)) {
        return GW_ERR_PROTOCOL;
    }
    update.quality = GW_QUALITY_GOOD;
    update.timestamp_ms = gw_time_data_ms();
    return (xQueueSend(q_point_update, &update, 0U) == pdTRUE) ? GW_OK : GW_ERR_FULL;
}

void poll_scheduler_init(void)
{
    memset(s_jobs, 0, sizeof(s_jobs));
    memset(s_next_due_ms, 0, sizeof(s_next_due_ms));
    s_job_count = 0U;
    s_transaction_id = 0U;
}

gw_err_t poll_scheduler_register(const poll_job_t *job)
{
    if ((job == NULL) || (job->id == 0U) || (job->device_id == 0U) ||
        (job->point_id == 0U) ||
        ((job->function_code != 0x03U) && (job->function_code != 0x04U)) ||
        (job->quantity == 0U) || (job->quantity > 125U) ||
        (job->interval_ms == 0U) ||
        (encoding_register_width(job->encoding) == 0U) ||
        ((uint32_t)job->register_offset + encoding_register_width(job->encoding) >
         job->quantity) || (poll_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }

    gw_device_t device;
    gw_point_t point;
    if ((device_manager_get(job->device_id, &device) != GW_OK) ||
        (point_db_get(job->point_id, &point) != GW_OK) ||
        (device.protocol != GW_PROTO_MODBUS_RTU) ||
        (device.interface_id != GW_IF_RS485_0) ||
        (point.device_id != job->device_id)) {
        return GW_ERR_STATE;
    }

    if (xSemaphoreTake(poll_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }
    if (find_job_index(job->id) >= 0) {
        (void)xSemaphoreGive(poll_db_mutex);
        return GW_ERR_STATE;
    }
    if (s_job_count >= GW_MAX_POLL_JOBS) {
        (void)xSemaphoreGive(poll_db_mutex);
        return GW_ERR_FULL;
    }

    s_jobs[s_job_count] = *job;
    s_next_due_ms[s_job_count] = gw_time_ms();
    ++s_job_count;
    (void)xSemaphoreGive(poll_db_mutex);
    return GW_OK;
}

gw_err_t poll_scheduler_upsert(const poll_job_t *job)
{
    if (job == NULL) return GW_ERR_PARAM;
    poll_job_t existing;
    gw_err_t found = poll_scheduler_get(job->id, &existing);
    if (found == GW_ERR_NOT_FOUND) return poll_scheduler_register(job);
    if (found != GW_OK) return found;

    if ((job->device_id == 0U) || (job->point_id == 0U) ||
        ((job->function_code != 0x03U) && (job->function_code != 0x04U)) ||
        (job->quantity == 0U) || (job->quantity > 125U) ||
        (job->interval_ms == 0U) ||
        (encoding_register_width(job->encoding) == 0U) ||
        ((uint32_t)job->register_offset + encoding_register_width(job->encoding) > job->quantity)) {
        return GW_ERR_PARAM;
    }
    gw_device_t device; gw_point_t point;
    if ((device_manager_get(job->device_id, &device) != GW_OK) ||
        (point_db_get(job->point_id, &point) != GW_OK) ||
        (device.protocol != GW_PROTO_MODBUS_RTU) ||
        (device.interface_id != GW_IF_RS485_0) ||
        (point.device_id != job->device_id)) return GW_ERR_STATE;

    if (xSemaphoreTake(poll_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) return GW_ERR_BUSY;
    int32_t idx = find_job_index(job->id);
    if (idx < 0) { (void)xSemaphoreGive(poll_db_mutex); return GW_ERR_NOT_FOUND; }
    s_jobs[idx] = *job;
    s_next_due_ms[idx] = gw_time_ms();
    (void)xSemaphoreGive(poll_db_mutex);
    return GW_OK;
}

gw_err_t poll_scheduler_remove(uint32_t id)
{
    if ((id == 0U) || (poll_db_mutex == NULL)) return GW_ERR_PARAM;
    if (xSemaphoreTake(poll_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) return GW_ERR_BUSY;
    int32_t idx = find_job_index(id);
    if (idx < 0) { (void)xSemaphoreGive(poll_db_mutex); return GW_ERR_NOT_FOUND; }
    for (uint32_t i = (uint32_t)idx; i + 1U < s_job_count; ++i) {
        s_jobs[i] = s_jobs[i + 1U];
        s_next_due_ms[i] = s_next_due_ms[i + 1U];
    }
    if (s_job_count != 0U) {
        --s_job_count;
        memset(&s_jobs[s_job_count], 0, sizeof(s_jobs[s_job_count]));
        s_next_due_ms[s_job_count] = 0U;
    }
    (void)xSemaphoreGive(poll_db_mutex);
    return GW_OK;
}

void poll_scheduler_reset(void)
{
    if ((poll_db_mutex != NULL) && (xSemaphoreTake(poll_db_mutex, pdMS_TO_TICKS(50U)) == pdTRUE)) {
        memset(s_jobs, 0, sizeof(s_jobs));
        memset(s_next_due_ms, 0, sizeof(s_next_due_ms));
        s_job_count = 0U;
        (void)xSemaphoreGive(poll_db_mutex);
    }
}

uint32_t poll_scheduler_snapshot(poll_job_t *out, uint32_t max_count)
{
    if ((out == NULL) || (max_count == 0U) || (poll_db_mutex == NULL)) return 0U;
    if (xSemaphoreTake(poll_db_mutex, pdMS_TO_TICKS(20U)) != pdTRUE) return 0U;
    uint32_t n = (s_job_count < max_count) ? s_job_count : max_count;
    memcpy(out, s_jobs, n * sizeof(out[0]));
    (void)xSemaphoreGive(poll_db_mutex);
    return n;
}

gw_err_t poll_scheduler_get(uint32_t id, poll_job_t *out)
{
    if ((id == 0U) || (out == NULL) || (poll_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(poll_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }
    int32_t idx = find_job_index(id);
    if (idx < 0) {
        (void)xSemaphoreGive(poll_db_mutex);
        return GW_ERR_NOT_FOUND;
    }
    *out = s_jobs[idx];
    (void)xSemaphoreGive(poll_db_mutex);
    return GW_OK;
}

bool poll_scheduler_handle_modbus_result(const modbus_result_t *result)
{
    uint32_t index;
    if ((result == NULL) || !context_to_index(result->context, &index)) {
        return false;
    }

    poll_job_t job;
    uint64_t ignored_due;
    if (!copy_job(index, &job, &ignored_due)) {
        return true;
    }

    if (result->result != GW_OK) {
        mark_job_failure(&job, result->result);
        return true;
    }

    gw_err_t err = apply_result(&job, result);
    if (err != GW_OK) {
        (void)point_db_set_quality(job.point_id, GW_QUALITY_BAD, gw_time_data_ms());
        GW_LOGW("POLL", "job=%lu decode failed=%ld",
                (unsigned long)job.id, (long)err);
    }
    return true;
}

static void poll_task(void *argument)
{
    (void)argument;

    (void)xEventGroupWaitBits(g_system_events,
                              EVT_CONFIG_READY | EVT_RS485_READY,
                              pdFALSE, pdTRUE, portMAX_DELAY);
    GW_LOGI("POLL", "scheduler started, jobs=%lu", (unsigned long)s_job_count);

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        gw_watchdog_beat(GW_WD_POLL);
        uint64_t now_ms = gw_time_ms();

        if ((xEventGroupGetBits(g_system_events) & EVT_CONFIG_UPDATING) != 0U) {
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(POLL_TASK_PERIOD_MS));
            continue;
        }

        if (rs485_bus_is_idle()) {
            for (uint32_t i = 0U; i < s_job_count; ++i) {
                poll_job_t job;
                uint64_t due;
                if (!copy_job(i, &job, &due) || !job.enabled || (now_ms < due)) {
                    continue;
                }

                gw_device_t device;
                if (device_manager_get(job.device_id, &device) != GW_OK) {
                    set_next_due(i, now_ms + job.interval_ms);
                    continue;
                }
                if ((device.state == DEVICE_DISABLED) ||
                    (device.protocol != GW_PROTO_MODBUS_RTU) ||
                    (device.interface_id != GW_IF_RS485_0) ||
                    (device.address == 0U) || (device.address > 247U)) {
                    set_next_due(i, now_ms + job.interval_ms);
                    continue;
                }

                gw_msg_block_t *request = gw_msg_alloc(0U);
                if (request == NULL) {
                    set_next_due(i, now_ms + POLL_SUBMIT_RETRY_MS);
                    break;
                }

                gw_err_t err = build_request(&job, (uint8_t)device.address,
                                             request);
                if (err != GW_OK) {
                    gw_msg_free(request);
                    set_next_due(i, now_ms + job.interval_ms);
                    continue;
                }

                rs485_transaction_t txn;
                memset(&txn, 0, sizeof(txn));
                txn.transaction_id = next_transaction_id();
                txn.device_id = job.device_id;
                txn.protocol = RS485_PROTO_MODBUS_RTU;
                txn.expected_rx_length = 0U; /* allow 5-byte Modbus exceptions */
                txn.timeout_ms = device.timeout_ms;
                txn.retry = device.retry;
                txn.request = request;
                txn.context = make_context(i);

                err = rs485_bus_submit(&txn, 0U);
                if (err == GW_OK) {
                    set_next_due(i, now_ms + job.interval_ms);
                } else {
                    gw_msg_free(request);
                    set_next_due(i, now_ms + POLL_SUBMIT_RETRY_MS);
                }
                break; /* one RS485 transaction at a time */
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(POLL_TASK_PERIOD_MS));
    }
}

void poll_scheduler_create(void)
{
    TaskHandle_t handle = xTaskCreateStatic(
        poll_task, "poll", POLL_TASK_STACK_WORDS, NULL,
        POLL_TASK_PRIORITY, s_poll_stack, &s_poll_task_cb);
    configASSERT(handle != NULL);
}
