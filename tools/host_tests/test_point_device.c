#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "point_db.h"
#include "device_manager.h"
#include "rtos_objects.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

QueueHandle_t q_point_update;
SemaphoreHandle_t point_db_mutex = (SemaphoreHandle_t)1;
SemaphoreHandle_t device_db_mutex = (SemaphoreHandle_t)1;
SemaphoreHandle_t poll_db_mutex = (SemaphoreHandle_t)1;

int main(void)
{
    point_db_init();
    device_manager_init();

    gw_point_t p = {0};
    p.id = 10U; p.device_id = 1U; p.type = GW_VALUE_F32;
    p.scale = 1.0f; p.offset = 0.0f; p.quality = GW_QUALITY_GOOD;
    p.value.f32 = NAN;
    CHECK(point_db_register(&p) == GW_ERR_PARAM);
    p.value.f32 = 1.0f;
    CHECK(point_db_register(&p) == GW_OK);
    CHECK(point_db_mark_device_quality(1U, GW_QUALITY_OFFLINE, 1234U) == GW_OK);
    gw_point_t got;
    CHECK(point_db_get(10U, &got) == GW_OK);
    CHECK(got.quality == GW_QUALITY_OFFLINE && got.timestamp_ms == 1234U && got.dirty);
    CHECK(got.revision != 0U);

    /* Northbound ACK must not clear a point that changed after its snapshot. */
    uint32_t snapshot_rev = got.revision;
    CHECK(point_db_set_quality(10U, GW_QUALITY_STALE, 1250U) == GW_OK);
    CHECK(point_db_ack_dirty(10U, snapshot_rev) == GW_ERR_STATE);
    CHECK(point_db_get(10U, &got) == GW_OK && got.dirty);
    snapshot_rev = got.revision;
    CHECK(point_db_ack_dirty(10U, snapshot_rev) == GW_OK);
    CHECK(point_db_get(10U, &got) == GW_OK && !got.dirty);

    gw_point_t point_snap[2] = {0};
    CHECK(point_db_snapshot(point_snap, 2U) == 1U);
    CHECK(point_snap[0].id == 10U && !point_snap[0].dirty);

    point_update_t u = {0};
    u.point_id = 10U; u.device_id = 1U; u.type = GW_VALUE_F32;
    u.quality = GW_QUALITY_GOOD; u.timestamp_ms = 1300U; u.value.f32 = INFINITY;
    CHECK(point_db_update(&u) == GW_ERR_PARAM);

    gw_device_t d = {0};
    d.id = 1U; d.protocol = GW_PROTO_MODBUS_RTU; d.interface_id = GW_IF_RS485_0;
    d.timeout_ms = 100U; d.state = DEVICE_INIT;
    CHECK(device_manager_register(&d) == GW_OK);
    device_manager_report_failure(1U, GW_ERR_TIMEOUT, 2000U);
    device_manager_report_failure(1U, GW_ERR_TIMEOUT, 2100U);
    device_manager_report_failure(1U, GW_ERR_TIMEOUT, 2200U);
    gw_device_t dg;
    CHECK(device_manager_get(1U, &dg) == GW_OK);
    CHECK(dg.state == DEVICE_OFFLINE && dg.consecutive_error == 3U);
    CHECK(point_db_get(10U, &got) == GW_OK);
    CHECK(got.quality == GW_QUALITY_OFFLINE && got.timestamp_ms == 2200U);
    device_manager_report_success(1U, 2300U);
    CHECK(device_manager_get(1U, &dg) == GW_OK);
    CHECK(dg.state == DEVICE_ONLINE && dg.consecutive_error == 0U);
    CHECK(point_db_get(10U, &got) == GW_OK);
    CHECK(got.quality == GW_QUALITY_STALE && got.timestamp_ms == 2300U);

    /* Protocol/interface bindings must be coherent so future adapters cannot
     * silently register a CAN device on RS485 or a raw RS485 device on ETH. */
    gw_device_t bad_binding = {0};
    bad_binding.id = 9U; bad_binding.protocol = GW_PROTO_CAN;
    bad_binding.interface_id = GW_IF_RS485_0; bad_binding.timeout_ms = 100U;
    CHECK(device_manager_register(&bad_binding) == GW_ERR_PARAM);
    bad_binding.protocol = GW_PROTO_RS485_RAW;
    bad_binding.interface_id = GW_IF_ETH_0;
    CHECK(device_manager_register(&bad_binding) == GW_ERR_PARAM);

    gw_device_t disabled = {0};
    disabled.id = 2U; disabled.protocol = GW_PROTO_CAN; disabled.interface_id = GW_IF_CANFD_0;
    disabled.timeout_ms = 100U; disabled.state = DEVICE_DISABLED;
    CHECK(device_manager_register(&disabled) == GW_OK);
    device_manager_report_failure(2U, GW_ERR_TIMEOUT, 3000U);
    device_manager_report_success(2U, 3100U);
    CHECK(device_manager_get(2U, &dg) == GW_OK);
    CHECK(dg.state == DEVICE_DISABLED && dg.success_count == 0U && dg.error_count == 0U);

    gw_device_t dev_snap[4] = {0};
    CHECK(device_manager_count() == 2U);
    CHECK(device_manager_snapshot(dev_snap, 4U) == 2U);
    CHECK(dev_snap[0].id == 1U && dev_snap[1].id == 2U);

    if (failures != 0) return 1;
    puts("point/device regression: PASS");
    return 0;
}
