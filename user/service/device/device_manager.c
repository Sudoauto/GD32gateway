#include "device_manager.h"
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "point_db.h"
#include "rtos_objects.h"

#define DEVICE_OFFLINE_THRESHOLD 3U

static gw_device_t s_devices[GW_MAX_DEVICES];
static uint32_t s_device_count;

static int32_t find_index(uint32_t id)
{
    for (uint32_t i = 0U; i < s_device_count; ++i) {
        if (s_devices[i].valid && (s_devices[i].id == id)) {
            return (int32_t)i;
        }
    }
    return -1;
}

void device_manager_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0U;
}

gw_err_t device_manager_register(const gw_device_t *device)
{
    if ((device == NULL) || (device->id == 0U) ||
        (device->protocol <= GW_PROTO_NONE) ||
        (device->protocol > GW_PROTO_RS485_RAW) ||
        (device->interface_id > GW_IF_ETH_0) ||
        (device->timeout_ms == 0U) || (device_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }

    bool binding_ok =
        ((device->protocol == GW_PROTO_MODBUS_RTU) && (device->interface_id == GW_IF_RS485_0)) ||
        ((device->protocol == GW_PROTO_RS485_RAW) && (device->interface_id == GW_IF_RS485_0)) ||
        ((device->protocol == GW_PROTO_CAN) && (device->interface_id == GW_IF_CANFD_0)) ||
        ((device->protocol == GW_PROTO_MODBUS_TCP) && (device->interface_id == GW_IF_ETH_0));
    if (!binding_ok) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(device_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }
    if (find_index(device->id) >= 0) {
        (void)xSemaphoreGive(device_db_mutex);
        return GW_ERR_STATE;
    }
    uint32_t slot = s_device_count;
    for (uint32_t i = 0U; i < s_device_count; ++i) {
        if (!s_devices[i].valid) { slot = i; break; }
    }
    if (slot >= GW_MAX_DEVICES) {
        (void)xSemaphoreGive(device_db_mutex);
        return GW_ERR_FULL;
    }

    s_devices[slot] = *device;
    s_devices[slot].name[GW_DEVICE_NAME_LEN - 1U] = '\0';
    s_devices[slot].valid = true;
    if (s_devices[slot].state != DEVICE_DISABLED) {
        s_devices[slot].state = DEVICE_INIT;
    }
    if (slot == s_device_count) ++s_device_count;
    (void)xSemaphoreGive(device_db_mutex);
    return GW_OK;
}


gw_err_t device_manager_upsert(const gw_device_t *device)
{
    if ((device == NULL) || (device->id == 0U) ||
        (device->protocol <= GW_PROTO_NONE) || (device->protocol > GW_PROTO_RS485_RAW) ||
        (device->interface_id > GW_IF_ETH_0) || (device->timeout_ms == 0U) ||
        (device_db_mutex == NULL)) return GW_ERR_PARAM;
    bool binding_ok =
        ((device->protocol == GW_PROTO_MODBUS_RTU) && (device->interface_id == GW_IF_RS485_0)) ||
        ((device->protocol == GW_PROTO_RS485_RAW) && (device->interface_id == GW_IF_RS485_0)) ||
        ((device->protocol == GW_PROTO_CAN) && (device->interface_id == GW_IF_CANFD_0)) ||
        ((device->protocol == GW_PROTO_MODBUS_TCP) && (device->interface_id == GW_IF_ETH_0));
    if (!binding_ok) return GW_ERR_PARAM;
    if (xSemaphoreTake(device_db_mutex, pdMS_TO_TICKS(20U)) != pdTRUE) return GW_ERR_BUSY;
    int32_t idx = find_index(device->id);
    if (idx >= 0) {
        device_state_t state = s_devices[idx].state;
        uint64_t last_seen = s_devices[idx].last_seen_ms;
        uint32_t ok = s_devices[idx].success_count, err = s_devices[idx].error_count;
        s_devices[idx] = *device;
        s_devices[idx].name[GW_DEVICE_NAME_LEN - 1U] = '\0';
        s_devices[idx].valid = true;
        if (device->state != DEVICE_DISABLED) {
            s_devices[idx].state = state;
            s_devices[idx].last_seen_ms = last_seen;
            s_devices[idx].success_count = ok;
            s_devices[idx].error_count = err;
        }
        (void)xSemaphoreGive(device_db_mutex);
        return GW_OK;
    }
    uint32_t slot=s_device_count;
    for(uint32_t i=0U;i<s_device_count;++i) if(!s_devices[i].valid){slot=i;break;}
    if(slot>=GW_MAX_DEVICES){(void)xSemaphoreGive(device_db_mutex);return GW_ERR_FULL;}
    s_devices[slot]=*device;s_devices[slot].name[GW_DEVICE_NAME_LEN-1U]='\0';s_devices[slot].valid=true;
    if(s_devices[slot].state!=DEVICE_DISABLED)s_devices[slot].state=DEVICE_INIT;
    if(slot==s_device_count)++s_device_count;
    (void)xSemaphoreGive(device_db_mutex);return GW_OK;
}

gw_err_t device_manager_remove(uint32_t id)
{
    if ((id == 0U) || (device_db_mutex == NULL)) return GW_ERR_PARAM;
    if (xSemaphoreTake(device_db_mutex, pdMS_TO_TICKS(20U)) != pdTRUE) return GW_ERR_BUSY;
    int32_t idx=find_index(id); if(idx<0){(void)xSemaphoreGive(device_db_mutex);return GW_ERR_NOT_FOUND;}
    memset(&s_devices[idx],0,sizeof(s_devices[idx]));
    while((s_device_count>0U)&&!s_devices[s_device_count-1U].valid)--s_device_count;
    (void)xSemaphoreGive(device_db_mutex);
    (void)point_db_mark_device_quality(id, GW_QUALITY_INVALID, 0U);
    return GW_OK;
}

void device_manager_reset(void)
{
    if(device_db_mutex==NULL)return;
    if(xSemaphoreTake(device_db_mutex,pdMS_TO_TICKS(50U))!=pdTRUE)return;
    memset(s_devices,0,sizeof(s_devices));s_device_count=0U;
    (void)xSemaphoreGive(device_db_mutex);
}

gw_err_t device_manager_get(uint32_t id, gw_device_t *out)
{
    if ((out == NULL) || (id == 0U) || (device_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(device_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }
    int32_t idx = find_index(id);
    if (idx < 0) {
        (void)xSemaphoreGive(device_db_mutex);
        return GW_ERR_NOT_FOUND;
    }
    *out = s_devices[idx];
    (void)xSemaphoreGive(device_db_mutex);
    return GW_OK;
}


gw_err_t device_manager_find_binding(gw_protocol_t protocol,
                                     gw_interface_id_t interface_id,
                                     uint16_t address,
                                     gw_device_t *out)
{
    if ((out == NULL) || (protocol <= GW_PROTO_NONE) ||
        (protocol > GW_PROTO_RS485_RAW) || (interface_id > GW_IF_ETH_0) ||
        (device_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(device_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }

    for (uint32_t i = 0U; i < s_device_count; ++i) {
        const gw_device_t *d = &s_devices[i];
        if (d->valid && (d->protocol == protocol) &&
            (d->interface_id == interface_id) && (d->address == address)) {
            *out = *d;
            (void)xSemaphoreGive(device_db_mutex);
            return GW_OK;
        }
    }

    (void)xSemaphoreGive(device_db_mutex);
    return GW_ERR_NOT_FOUND;
}

uint32_t device_manager_count(void)
{
    if (device_db_mutex == NULL) {
        return 0U;
    }
    if (xSemaphoreTake(device_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return 0U;
    }
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < s_device_count; ++i) if (s_devices[i].valid) ++count;
    (void)xSemaphoreGive(device_db_mutex);
    return count;
}

uint32_t device_manager_snapshot(gw_device_t *out, uint32_t max_count)
{
    if ((out == NULL) || (max_count == 0U) || (device_db_mutex == NULL)) {
        return 0U;
    }
    if (xSemaphoreTake(device_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return 0U;
    }

    uint32_t n = 0U;
    for (uint32_t i = 0U; (i < s_device_count) && (n < max_count); ++i) {
        if (s_devices[i].valid) {
            out[n++] = s_devices[i];
        }
    }
    (void)xSemaphoreGive(device_db_mutex);
    return n;
}

void device_manager_report_success(uint32_t id, uint64_t now_ms)
{
    if (device_db_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(device_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return;
    }
    int32_t idx = find_index(id);
    if (idx < 0) {
        (void)xSemaphoreGive(device_db_mutex);
        return;
    }

    gw_device_t *d = &s_devices[idx];
    if (d->state == DEVICE_DISABLED) {
        (void)xSemaphoreGive(device_db_mutex);
        return;
    }
    bool was_offline = (d->state == DEVICE_OFFLINE) ||
                       (d->state == DEVICE_ERROR);
    ++d->success_count;
    d->consecutive_error = 0U;
    d->last_seen_ms = now_ms;
    d->last_error = GW_OK;
    d->last_error_ms = 0U;
    d->state = DEVICE_ONLINE;
    (void)xSemaphoreGive(device_db_mutex);

    /* Do not hold the device mutex while entering Point DB. */
    if (was_offline) {
        (void)point_db_mark_device_quality(id, GW_QUALITY_STALE, now_ms);
    }
}

void device_manager_report_failure(uint32_t id, gw_err_t reason, uint64_t now_ms)
{
    if (device_db_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(device_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return;
    }
    int32_t idx = find_index(id);
    if (idx < 0) {
        (void)xSemaphoreGive(device_db_mutex);
        return;
    }

    gw_device_t *d = &s_devices[idx];
    if (d->state == DEVICE_DISABLED) {
        (void)xSemaphoreGive(device_db_mutex);
        return;
    }
    ++d->error_count;
    ++d->consecutive_error;
    d->last_error = reason;
    d->last_error_ms = now_ms;
    bool went_offline = d->consecutive_error >= DEVICE_OFFLINE_THRESHOLD;
    if (went_offline) {
        d->state = DEVICE_OFFLINE;
    } else {
        d->state = DEVICE_ERROR;
    }
    (void)xSemaphoreGive(device_db_mutex);

    if (went_offline) {
        (void)point_db_mark_device_quality(id, GW_QUALITY_OFFLINE, now_ms);
    }
}
