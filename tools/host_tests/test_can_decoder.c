#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "can_decoder.h"
#include "gw_types.h"
#include "queue.h"
#include "semphr.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

QueueHandle_t q_point_update = (QueueHandle_t)1;
SemaphoreHandle_t config_db_mutex = (SemaphoreHandle_t)1;
static gw_device_t s_device;
static gw_point_t s_point;
static point_update_t s_last_update;
static unsigned s_queue_count;
static unsigned s_success_count;
static uint64_t s_now = 1000U;

BaseType_t host_xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)q; (void)wait;
    s_last_update = *(const point_update_t *)item;
    ++s_queue_count;
    return pdTRUE;
}

gw_err_t device_manager_get(uint32_t id, gw_device_t *out)
{
    if (id != s_device.id) return GW_ERR_NOT_FOUND;
    *out = s_device;
    return GW_OK;
}
void device_manager_report_success(uint32_t id, uint64_t now_ms)
{
    if (id == s_device.id) {
        ++s_success_count;
        s_device.last_seen_ms = now_ms;
        s_device.state = DEVICE_ONLINE;
    }
}
void device_manager_report_failure(uint32_t id, gw_err_t reason, uint64_t now_ms)
{
    (void)id; (void)reason; (void)now_ms;
}
gw_err_t point_db_get(uint32_t id, gw_point_t *out)
{
    if (id != s_point.id) return GW_ERR_NOT_FOUND;
    *out = s_point;
    return GW_OK;
}
gw_err_t point_db_mark_device_quality(uint32_t id, gw_quality_t q, uint64_t ts)
{
    (void)id; (void)q; (void)ts;
    return GW_OK;
}
uint64_t gw_time_ms(void) { return s_now++; }
uint64_t gw_time_data_ms(void) { return gw_time_ms(); }

static void setup_point(gw_value_type_t type)
{
    memset(&s_device, 0, sizeof(s_device));
    memset(&s_point, 0, sizeof(s_point));
    memset(&s_last_update, 0, sizeof(s_last_update));
    s_queue_count = 0U;
    s_success_count = 0U;
    s_device.id = 1U;
    s_device.protocol = GW_PROTO_CAN;
    s_device.interface_id = GW_IF_CANFD_0;
    s_device.timeout_ms = 1000U;
    s_device.state = DEVICE_INIT;
    s_point.id = 100U;
    s_point.device_id = 1U;
    s_point.type = type;
    s_point.scale = 1.0f;
    s_point.offset = 0.0f;
}

int main(void)
{
    setup_point(GW_VALUE_U32);
    can_decoder_init();
    can_signal_map_t map = {0};
    map.id = 1U; map.device_id = 1U; map.point_id = 100U; map.can_id = 0x123U;
    map.encoding = CAN_SIGNAL_U32; map.endian = CAN_ENDIAN_BIG; map.enabled = true;
    CHECK(can_decoder_register(&map) == GW_OK);

    canfd_frame_t frame = {0};
    frame.id = 0x123U; frame.len = 4U;
    frame.data[0] = 0xFFU; frame.data[1] = 0xFFU; frame.data[2] = 0xFFU; frame.data[3] = 0xFFU;
    can_decoder_process(&frame);
    CHECK(s_queue_count == 1U);
    CHECK(s_last_update.type == GW_VALUE_U32);
    CHECK(s_last_update.value.u32 == UINT32_MAX);
    CHECK(s_success_count == 1U);

    setup_point(GW_VALUE_F32);
    can_decoder_init();
    map.id = 2U; map.encoding = CAN_SIGNAL_F32;
    CHECK(can_decoder_register(&map) == GW_OK);
    frame.data[0] = 0x7FU; frame.data[1] = 0xC0U; frame.data[2] = 0U; frame.data[3] = 0U; /* NaN */
    can_decoder_process(&frame);
    CHECK(s_queue_count == 0U);
    can_decoder_stats_t stats;
    can_decoder_get_stats(&stats);
    CHECK(stats.schema_error == 1U);

    setup_point(GW_VALUE_U16);
    can_decoder_init();
    map.id = 3U; map.encoding = CAN_SIGNAL_U16; map.endian = CAN_ENDIAN_BIG;
    map.byte_offset = 63U;
    CHECK(can_decoder_register(&map) == GW_ERR_PARAM);
    map.byte_offset = 0U;
    map.endian = (can_endian_t)99;
    CHECK(can_decoder_register(&map) == GW_ERR_PARAM);

    if (failures != 0) return 1;
    puts("can decoder regression: PASS");
    return 0;
}
