#include "point_db.h"
#include <float.h>
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "rtos_objects.h"

static gw_point_t s_points[GW_MAX_POINTS];
static uint32_t s_point_count;

static int32_t find_index(uint32_t point_id)
{
    for (uint32_t i = 0U; i < s_point_count; ++i) {
        if (s_points[i].valid && (s_points[i].id == point_id)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static bool valid_value_type(gw_value_type_t type)
{
    return (type >= GW_VALUE_BOOL) && (type <= GW_VALUE_F64);
}

static bool valid_quality(gw_quality_t quality)
{
    return (quality >= GW_QUALITY_GOOD) && (quality <= GW_QUALITY_INVALID);
}

static bool finite_float(float value)
{
    return (value == value) && (value <= FLT_MAX) && (value >= -FLT_MAX);
}

static bool finite_double(double value)
{
    return (value == value) && (value <= DBL_MAX) && (value >= -DBL_MAX);
}

static uint32_t next_revision(uint32_t current)
{
    ++current;
    /* Reserve zero as the initial/unpublished revision. */
    return (current == 0U) ? 1U : current;
}

static bool valid_value(gw_value_type_t type, const gw_value_t *value)
{
    if (value == NULL) {
        return false;
    }
    if (type == GW_VALUE_F32) {
        return finite_float(value->f32);
    }
    if (type == GW_VALUE_F64) {
        return finite_double(value->f64);
    }
    return true;
}

void point_db_init(void)
{
    memset(s_points, 0, sizeof(s_points));
    s_point_count = 0U;
}

gw_err_t point_db_register(const gw_point_t *point)
{
    if ((point == NULL) || (point->id == 0U) || (point->device_id == 0U) ||
        !valid_value_type(point->type) || !valid_quality(point->quality) ||
        !valid_value(point->type, &point->value) ||
        !finite_float(point->scale) || !finite_float(point->offset) ||
        (point_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }

    if (find_index(point->id) >= 0) {
        (void)xSemaphoreGive(point_db_mutex);
        return GW_ERR_STATE;
    }
    uint32_t slot = s_point_count;
    for (uint32_t i = 0U; i < s_point_count; ++i) if (!s_points[i].valid) { slot = i; break; }
    if (slot >= GW_MAX_POINTS) {
        (void)xSemaphoreGive(point_db_mutex);
        return GW_ERR_FULL;
    }

    s_points[slot] = *point;
    s_points[slot].name[GW_POINT_NAME_LEN - 1U] = '\0';
    s_points[slot].revision = 0U;
    s_points[slot].valid = true;
    s_points[slot].dirty = false;
    if (slot == s_point_count) ++s_point_count;

    (void)xSemaphoreGive(point_db_mutex);
    return GW_OK;
}


gw_err_t point_db_upsert(const gw_point_t *point)
{
    if ((point == NULL) || (point->id == 0U) || (point->device_id == 0U) ||
        !valid_value_type(point->type) || !valid_quality(point->quality) ||
        !finite_float(point->scale) || !finite_float(point->offset) ||
        (point_db_mutex == NULL)) return GW_ERR_PARAM;
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(20U)) != pdTRUE) return GW_ERR_BUSY;
    int32_t idx=find_index(point->id);
    if(idx>=0){
        gw_value_t value=s_points[idx].value;gw_quality_t q=s_points[idx].quality;
        uint64_t ts=s_points[idx].timestamp_ms;uint32_t rev=s_points[idx].revision;bool dirty=s_points[idx].dirty;
        s_points[idx]=*point;s_points[idx].name[GW_POINT_NAME_LEN-1U]='\0';s_points[idx].valid=true;
        s_points[idx].value=value;s_points[idx].quality=q;s_points[idx].timestamp_ms=ts;s_points[idx].revision=rev;s_points[idx].dirty=dirty;
        (void)xSemaphoreGive(point_db_mutex);return GW_OK;
    }
    uint32_t slot=s_point_count;for(uint32_t i=0U;i<s_point_count;++i)if(!s_points[i].valid){slot=i;break;}
    if(slot>=GW_MAX_POINTS){(void)xSemaphoreGive(point_db_mutex);return GW_ERR_FULL;}
    s_points[slot]=*point;s_points[slot].name[GW_POINT_NAME_LEN-1U]='\0';s_points[slot].revision=0U;s_points[slot].valid=true;s_points[slot].dirty=false;
    if(slot==s_point_count)++s_point_count;(void)xSemaphoreGive(point_db_mutex);return GW_OK;
}

gw_err_t point_db_remove(uint32_t point_id)
{
    if((point_id==0U)||(point_db_mutex==NULL))return GW_ERR_PARAM;
    if(xSemaphoreTake(point_db_mutex,pdMS_TO_TICKS(20U))!=pdTRUE)return GW_ERR_BUSY;
    int32_t idx=find_index(point_id);if(idx<0){(void)xSemaphoreGive(point_db_mutex);return GW_ERR_NOT_FOUND;}
    memset(&s_points[idx],0,sizeof(s_points[idx]));while((s_point_count>0U)&&!s_points[s_point_count-1U].valid)--s_point_count;
    (void)xSemaphoreGive(point_db_mutex);return GW_OK;
}

void point_db_reset(void)
{
    if(point_db_mutex==NULL)return;if(xSemaphoreTake(point_db_mutex,pdMS_TO_TICKS(50U))!=pdTRUE)return;
    memset(s_points,0,sizeof(s_points));s_point_count=0U;(void)xSemaphoreGive(point_db_mutex);
}

gw_err_t point_db_update(const point_update_t *update)
{
    if ((update == NULL) || (update->point_id == 0U) ||
        (update->device_id == 0U) || !valid_value_type(update->type) ||
        !valid_quality(update->quality) ||
        !valid_value(update->type, &update->value) ||
        (point_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }

    int32_t idx = find_index(update->point_id);
    if (idx < 0) {
        (void)xSemaphoreGive(point_db_mutex);
        return GW_ERR_NOT_FOUND;
    }

    gw_point_t *p = &s_points[idx];
    /* Point identity/schema belong to configuration, not to a producer. */
    if ((update->device_id != p->device_id) || (update->type != p->type)) {
        (void)xSemaphoreGive(point_db_mutex);
        return GW_ERR_STATE;
    }

    p->value = update->value;
    p->quality = update->quality;
    p->timestamp_ms = update->timestamp_ms;
    p->revision = next_revision(p->revision);
    p->dirty = true;

    (void)xSemaphoreGive(point_db_mutex);
    return GW_OK;
}

gw_err_t point_db_get(uint32_t point_id, gw_point_t *out)
{
    if ((out == NULL) || (point_id == 0U) || (point_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }

    int32_t idx = find_index(point_id);
    if (idx < 0) {
        (void)xSemaphoreGive(point_db_mutex);
        return GW_ERR_NOT_FOUND;
    }
    *out = s_points[idx];
    (void)xSemaphoreGive(point_db_mutex);
    return GW_OK;
}

gw_err_t point_db_set_quality(uint32_t point_id, gw_quality_t quality,
                              uint64_t timestamp_ms)
{
    if ((point_id == 0U) || !valid_quality(quality) ||
        (point_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }

    int32_t idx = find_index(point_id);
    if (idx < 0) {
        (void)xSemaphoreGive(point_db_mutex);
        return GW_ERR_NOT_FOUND;
    }
    s_points[idx].quality = quality;
    s_points[idx].timestamp_ms = timestamp_ms;
    s_points[idx].revision = next_revision(s_points[idx].revision);
    s_points[idx].dirty = true;

    (void)xSemaphoreGive(point_db_mutex);
    return GW_OK;
}

gw_err_t point_db_mark_device_quality(uint32_t device_id, gw_quality_t quality,
                                      uint64_t timestamp_ms)
{
    if ((device_id == 0U) || !valid_quality(quality) ||
        (point_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }

    for (uint32_t i = 0U; i < s_point_count; ++i) {
        if (s_points[i].valid && (s_points[i].device_id == device_id)) {
            s_points[i].quality = quality;
            s_points[i].timestamp_ms = timestamp_ms;
            s_points[i].revision = next_revision(s_points[i].revision);
            s_points[i].dirty = true;
        }
    }
    (void)xSemaphoreGive(point_db_mutex);
    return GW_OK;
}

uint32_t point_db_count(void)
{
    if (point_db_mutex == NULL) {
        return 0U;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return 0U;
    }
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < s_point_count; ++i) if (s_points[i].valid) ++count;
    (void)xSemaphoreGive(point_db_mutex);
    return count;
}

uint32_t point_db_snapshot(gw_point_t *out, uint32_t max_count)
{
    if ((out == NULL) || (max_count == 0U) || (point_db_mutex == NULL)) {
        return 0U;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return 0U;
    }

    uint32_t n = 0U;
    for (uint32_t i = 0U; (i < s_point_count) && (n < max_count); ++i) {
        if (s_points[i].valid) {
            out[n++] = s_points[i];
        }
    }
    (void)xSemaphoreGive(point_db_mutex);
    return n;
}


uint32_t point_db_snapshot_range(uint32_t offset, gw_point_t *out, uint32_t max_count)
{
    if ((out == NULL) || (max_count == 0U) || (point_db_mutex == NULL)) {
        return 0U;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return 0U;
    }

    uint32_t skipped = 0U;
    uint32_t n = 0U;
    for (uint32_t i = 0U; (i < s_point_count) && (n < max_count); ++i) {
        if (!s_points[i].valid) {
            continue;
        }
        if (skipped < offset) {
            ++skipped;
            continue;
        }
        out[n++] = s_points[i];
    }
    (void)xSemaphoreGive(point_db_mutex);
    return n;
}

uint32_t point_db_collect_dirty(gw_point_t *out, uint32_t max_count,
                                bool clear_dirty)
{
    if ((out == NULL) || (max_count == 0U) || (point_db_mutex == NULL)) {
        return 0U;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return 0U;
    }

    uint32_t n = 0U;
    for (uint32_t i = 0U; (i < s_point_count) && (n < max_count); ++i) {
        if (s_points[i].valid && s_points[i].dirty) {
            out[n++] = s_points[i];
            if (clear_dirty) {
                s_points[i].dirty = false;
            }
        }
    }
    (void)xSemaphoreGive(point_db_mutex);
    return n;
}


gw_err_t point_db_ack_dirty(uint32_t point_id, uint32_t expected_revision)
{
    if ((point_id == 0U) || (expected_revision == 0U) ||
        (point_db_mutex == NULL)) {
        return GW_ERR_PARAM;
    }
    if (xSemaphoreTake(point_db_mutex, pdMS_TO_TICKS(10U)) != pdTRUE) {
        return GW_ERR_BUSY;
    }

    int32_t idx = find_index(point_id);
    if (idx < 0) {
        (void)xSemaphoreGive(point_db_mutex);
        return GW_ERR_NOT_FOUND;
    }

    gw_err_t result = GW_OK;
    if (s_points[idx].revision != expected_revision) {
        /* A newer value/quality update arrived after the northbound snapshot.
         * Keep dirty asserted so the newer state cannot be lost. */
        result = GW_ERR_STATE;
    } else {
        s_points[idx].dirty = false;
    }

    (void)xSemaphoreGive(point_db_mutex);
    return result;
}
