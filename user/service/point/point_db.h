#ifndef POINT_DB_H
#define POINT_DB_H

#include <stdint.h>
#include "gw_error.h"
#include "gw_types.h"

void point_db_init(void);
gw_err_t point_db_register(const gw_point_t *point);
gw_err_t point_db_upsert(const gw_point_t *point);
gw_err_t point_db_remove(uint32_t point_id);
void point_db_reset(void);
gw_err_t point_db_update(const point_update_t *update);
gw_err_t point_db_get(uint32_t point_id, gw_point_t *out);
gw_err_t point_db_mark_device_quality(uint32_t device_id, gw_quality_t quality,
                                      uint64_t timestamp_ms);
gw_err_t point_db_set_quality(uint32_t point_id, gw_quality_t quality, uint64_t timestamp_ms);
uint32_t point_db_count(void);
/* Copy a stable, non-destructive snapshot for GUI/northbound readers. */
uint32_t point_db_snapshot(gw_point_t *out, uint32_t max_count);
/* Copy a stable page of points starting at a valid-point offset. */
uint32_t point_db_snapshot_range(uint32_t offset, gw_point_t *out, uint32_t max_count);
uint32_t point_db_collect_dirty(gw_point_t *out, uint32_t max_count, bool clear_dirty);
/* ACK a previously collected dirty snapshot. Dirty is cleared only when the
 * point has not changed since expected_revision was captured. */
gw_err_t point_db_ack_dirty(uint32_t point_id, uint32_t expected_revision);

#endif
