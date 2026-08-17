#include "can_decoder.h"
#include <float.h>
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "device_manager.h"
#include "gw_time.h"
#include "point_db.h"
#include "rtos_objects.h"

#define CAN_MAX_SIGNAL_MAPS      64U
#define CAN_MAX_WATCHED_DEVICES  GW_MAX_DEVICES

typedef struct {
    uint32_t device_id;
    uint64_t first_deadline_ms;
    uint64_t next_report_ms;
    bool valid;
} can_device_watch_t;

static can_signal_map_t s_maps[CAN_MAX_SIGNAL_MAPS];
static uint32_t s_map_count;
static can_device_watch_t s_watch[CAN_MAX_WATCHED_DEVICES];
static can_decoder_stats_t s_stats;

static uint8_t encoding_size(can_signal_encoding_t encoding)
{
    switch (encoding) {
    case CAN_SIGNAL_U8:
    case CAN_SIGNAL_I8:  return 1U;
    case CAN_SIGNAL_U16:
    case CAN_SIGNAL_I16: return 2U;
    case CAN_SIGNAL_U32:
    case CAN_SIGNAL_I32:
    case CAN_SIGNAL_F32: return 4U;
    default: return 0U;
    }
}

static uint32_t read_u32(const uint8_t *p, uint8_t size, can_endian_t endian)
{
    uint32_t v = 0U;
    if (endian == CAN_ENDIAN_BIG) {
        for (uint8_t i = 0U; i < size; ++i) {
            v = (v << 8U) | (uint32_t)p[i];
        }
    } else {
        for (uint8_t i = 0U; i < size; ++i) {
            v |= ((uint32_t)p[i] << (8U * i));
        }
    }
    return v;
}

static bool watch_register(uint32_t device_id, uint32_t timeout_ms)
{
    for (uint32_t i = 0U; i < CAN_MAX_WATCHED_DEVICES; ++i) {
        if (s_watch[i].valid && (s_watch[i].device_id == device_id)) {
            return true;
        }
    }
    for (uint32_t i = 0U; i < CAN_MAX_WATCHED_DEVICES; ++i) {
        if (!s_watch[i].valid) {
            uint64_t now = gw_time_ms();
            s_watch[i].device_id = device_id;
            s_watch[i].first_deadline_ms = now + timeout_ms;
            s_watch[i].next_report_ms = s_watch[i].first_deadline_ms;
            s_watch[i].valid = true;
            return true;
        }
    }
    return false;
}

static bool frame_matches(const can_signal_map_t *map, const canfd_frame_t *frame)
{
    return map->enabled && (map->can_id == frame->id) &&
           (map->extended == frame->extended) &&
           (!map->require_fd || frame->fd);
}

static bool finite_double(double value)
{
    return (value == value) && (value <= DBL_MAX) && (value >= -DBL_MAX);
}

static bool value_from_double(gw_value_type_t type, double value, gw_value_t *out)
{
    if ((out == NULL) || !finite_double(value)) {
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

static bool build_update(const can_signal_map_t *map, const canfd_frame_t *frame,
                         point_update_t *update)
{
    uint8_t size = encoding_size(map->encoding);
    if ((size == 0U) || ((uint16_t)map->byte_offset + size > frame->len)) {
        ++s_stats.invalid_length;
        return false;
    }

    gw_point_t point;
    if (point_db_get(map->point_id, &point) != GW_OK ||
        point.device_id != map->device_id) {
        ++s_stats.schema_error;
        return false;
    }

    const uint8_t *p = &frame->data[map->byte_offset];
    uint32_t raw = read_u32(p, size, map->endian);
    double engineering;

    switch (map->encoding) {
    case CAN_SIGNAL_U8:  engineering = (double)(uint8_t)raw; break;
    case CAN_SIGNAL_I8:  engineering = (double)(int8_t)raw; break;
    case CAN_SIGNAL_U16: engineering = (double)(uint16_t)raw; break;
    case CAN_SIGNAL_I16: engineering = (double)(int16_t)raw; break;
    case CAN_SIGNAL_U32: engineering = (double)raw; break;
    case CAN_SIGNAL_I32: engineering = (double)(int32_t)raw; break;
    case CAN_SIGNAL_F32: {
        float f;
        memcpy(&f, &raw, sizeof(f));
        engineering = (double)f;
        break;
    }
    default:
        return false;
    }
    engineering = engineering * (double)point.scale + (double)point.offset;

    memset(update, 0, sizeof(*update));
    update->point_id = map->point_id;
    update->device_id = map->device_id;
    update->type = point.type;
    update->quality = GW_QUALITY_GOOD;
    update->timestamp_ms = gw_time_data_ms();

    if (!value_from_double(point.type, engineering, &update->value)) {
        ++s_stats.schema_error;
        return false;
    }
    return true;
}


static gw_err_t validate_map(const can_signal_map_t *map)
{
    uint8_t signal_size = (map != NULL) ? encoding_size(map->encoding) : 0U;
    if ((map == NULL) || (map->id == 0U) || (map->device_id == 0U) ||
        (map->point_id == 0U) || (signal_size == 0U) ||
        ((map->endian != CAN_ENDIAN_BIG) && (map->endian != CAN_ENDIAN_LITTLE)) ||
        ((uint16_t)map->byte_offset + signal_size > CANFD_MAX_DATA_BYTES) ||
        (map->can_id > (map->extended ? 0x1FFFFFFFU : 0x7FFU))) {
        return GW_ERR_PARAM;
    }
    gw_device_t device;
    gw_point_t point;
    if ((device_manager_get(map->device_id, &device) != GW_OK) ||
        (point_db_get(map->point_id, &point) != GW_OK) ||
        (device.protocol != GW_PROTO_CAN) ||
        (device.interface_id != GW_IF_CANFD_0) ||
        (point.device_id != map->device_id)) {
        return GW_ERR_STATE;
    }
    return GW_OK;
}

static void rebuild_watch_locked(void)
{
    memset(s_watch, 0, sizeof(s_watch));
    for (uint32_t i = 0U; i < s_map_count; ++i) {
        if (!s_maps[i].enabled) continue;
        gw_device_t device;
        if ((device_manager_get(s_maps[i].device_id, &device) == GW_OK) &&
            (device.timeout_ms != 0U)) {
            (void)watch_register(device.id, device.timeout_ms);
        }
    }
}

void can_decoder_init(void)
{
    memset(s_maps, 0, sizeof(s_maps));
    memset(s_watch, 0, sizeof(s_watch));
    memset(&s_stats, 0, sizeof(s_stats));
    s_map_count = 0U;
}

gw_err_t can_decoder_register(const can_signal_map_t *map)
{
    gw_err_t v = validate_map(map);
    if (v != GW_OK) return v;
    if (config_db_mutex == NULL) return GW_ERR_STATE;
    if (xSemaphoreTake(config_db_mutex, pdMS_TO_TICKS(20U)) != pdTRUE) return GW_ERR_BUSY;
    if (s_map_count >= CAN_MAX_SIGNAL_MAPS) {
        (void)xSemaphoreGive(config_db_mutex); return GW_ERR_FULL;
    }
    for (uint32_t i = 0U; i < s_map_count; ++i) {
        if (s_maps[i].id == map->id) {
            (void)xSemaphoreGive(config_db_mutex); return GW_ERR_STATE;
        }
    }
    s_maps[s_map_count++] = *map;
    rebuild_watch_locked();
    (void)xSemaphoreGive(config_db_mutex);
    return GW_OK;
}

gw_err_t can_decoder_upsert(const can_signal_map_t *map)
{
    gw_err_t v=validate_map(map); if(v!=GW_OK)return v;
    if(config_db_mutex==NULL)return GW_ERR_STATE;
    if(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(20U))!=pdTRUE)return GW_ERR_BUSY;
    for(uint32_t i=0U;i<s_map_count;++i){
        if(s_maps[i].id==map->id){s_maps[i]=*map;rebuild_watch_locked();(void)xSemaphoreGive(config_db_mutex);return GW_OK;}
    }
    if(s_map_count>=CAN_MAX_SIGNAL_MAPS){(void)xSemaphoreGive(config_db_mutex);return GW_ERR_FULL;}
    s_maps[s_map_count++]=*map;rebuild_watch_locked();(void)xSemaphoreGive(config_db_mutex);return GW_OK;
}

gw_err_t can_decoder_remove(uint32_t map_id)
{
    if((map_id==0U)||(config_db_mutex==NULL))return GW_ERR_PARAM;
    if(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(20U))!=pdTRUE)return GW_ERR_BUSY;
    for(uint32_t i=0U;i<s_map_count;++i){
        if(s_maps[i].id==map_id){
            for(uint32_t j=i+1U;j<s_map_count;++j)s_maps[j-1U]=s_maps[j];
            --s_map_count;memset(&s_maps[s_map_count],0,sizeof(s_maps[0]));rebuild_watch_locked();
            (void)xSemaphoreGive(config_db_mutex);return GW_OK;
        }
    }
    (void)xSemaphoreGive(config_db_mutex);return GW_ERR_NOT_FOUND;
}

void can_decoder_reset(void)
{
    if(config_db_mutex==NULL)return;
    if(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(50U))!=pdTRUE)return;
    memset(s_maps,0,sizeof(s_maps));memset(s_watch,0,sizeof(s_watch));s_map_count=0U;
    (void)xSemaphoreGive(config_db_mutex);
}

uint32_t can_decoder_snapshot(can_signal_map_t *out,uint32_t max_count)
{
    if((out==NULL)||(max_count==0U)||(config_db_mutex==NULL))return 0U;
    if(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(20U))!=pdTRUE)return 0U;
    uint32_t n=(s_map_count<max_count)?s_map_count:max_count;memcpy(out,s_maps,n*sizeof(s_maps[0]));
    (void)xSemaphoreGive(config_db_mutex);return n;
}

void can_decoder_process(const canfd_frame_t *frame)
{
    if (frame == NULL) return;
    ++s_stats.frames_seen;
    can_signal_map_t local[CAN_MAX_SIGNAL_MAPS];
    uint32_t local_count=0U;
    if((config_db_mutex!=NULL)&&(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(5U))==pdTRUE)){
        local_count=s_map_count;memcpy(local,s_maps,local_count*sizeof(local[0]));(void)xSemaphoreGive(config_db_mutex);
    } else {
        ++s_stats.schema_error; return;
    }
    bool frame_matched=false;
    uint32_t success_devices[CAN_MAX_WATCHED_DEVICES]={0U};uint8_t success_device_count=0U;
    for(uint32_t i=0U;i<local_count;++i){
        const can_signal_map_t *map=&local[i];if(!frame_matches(map,frame))continue;frame_matched=true;
        point_update_t update;if(!build_update(map,frame,&update))continue;++s_stats.signals_decoded;
        if(xQueueSend(q_point_update,&update,0U)==pdTRUE)++s_stats.point_updates_queued;else ++s_stats.point_update_drop;
        bool seen=false;for(uint8_t d=0U;d<success_device_count;++d)if(success_devices[d]==map->device_id){seen=true;break;}
        if(!seen&&success_device_count<CAN_MAX_WATCHED_DEVICES)success_devices[success_device_count++]=map->device_id;
    }
    if(frame_matched)++s_stats.frames_matched;
    uint64_t now=gw_time_ms();
    for(uint8_t d=0U;d<success_device_count;++d){device_manager_report_success(success_devices[d],now);}
    if((config_db_mutex!=NULL)&&(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(5U))==pdTRUE)){
        for(uint8_t d=0U;d<success_device_count;++d){
            for(uint32_t w=0U;w<CAN_MAX_WATCHED_DEVICES;++w){if(s_watch[w].valid&&s_watch[w].device_id==success_devices[d]){gw_device_t dev;if(device_manager_get(success_devices[d],&dev)==GW_OK){s_watch[w].next_report_ms=now+dev.timeout_ms;s_watch[w].first_deadline_ms=s_watch[w].next_report_ms;}break;}}
        }
        (void)xSemaphoreGive(config_db_mutex);
    }
}

void can_decoder_maintenance(uint64_t now_ms)
{
    can_device_watch_t watch[CAN_MAX_WATCHED_DEVICES];
    memset(watch, 0, sizeof(watch));
    if ((config_db_mutex == NULL) ||
        (xSemaphoreTake(config_db_mutex, pdMS_TO_TICKS(5U)) != pdTRUE)) {
        ++s_stats.schema_error;
        return;
    }
    memcpy(watch, s_watch, sizeof(watch));
    (void)xSemaphoreGive(config_db_mutex);

    for (uint32_t i = 0U; i < CAN_MAX_WATCHED_DEVICES; ++i) {
        if (!watch[i].valid) continue;
        gw_device_t dev;
        if (device_manager_get(watch[i].device_id, &dev) != GW_OK ||
            dev.state == DEVICE_DISABLED || dev.timeout_ms == 0U) continue;

        uint64_t deadline = (dev.last_seen_ms == 0U) ?
                            watch[i].first_deadline_ms :
                            (dev.last_seen_ms + dev.timeout_ms);
        if ((now_ms < deadline) || (now_ms < watch[i].next_report_ms)) continue;

        bool was_offline = (dev.state == DEVICE_OFFLINE);
        device_manager_report_failure(dev.id, GW_ERR_TIMEOUT, now_ms);

        if ((config_db_mutex != NULL) &&
            (xSemaphoreTake(config_db_mutex, pdMS_TO_TICKS(5U)) == pdTRUE)) {
            for (uint32_t w = 0U; w < CAN_MAX_WATCHED_DEVICES; ++w) {
                if (s_watch[w].valid && s_watch[w].device_id == dev.id) {
                    s_watch[w].next_report_ms = now_ms + dev.timeout_ms;
                    break;
                }
            }
            (void)xSemaphoreGive(config_db_mutex);
        }

        gw_device_t after;
        if (device_manager_get(dev.id, &after) == GW_OK) {
            if (after.state == DEVICE_OFFLINE) {
                if (!was_offline) ++s_stats.offline_events;
            } else {
                (void)point_db_mark_device_quality(dev.id, GW_QUALITY_STALE, now_ms);
                ++s_stats.stale_events;
            }
        }
    }
}

void can_decoder_get_stats(can_decoder_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}
