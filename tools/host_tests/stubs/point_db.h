#ifndef HOST_POINT_DB_H
#define HOST_POINT_DB_H
#include "gw_error.h"
#include "gw_types.h"
gw_err_t point_db_get(uint32_t point_id, gw_point_t *out);
gw_err_t point_db_mark_device_quality(uint32_t device_id, gw_quality_t quality,
                                      uint64_t timestamp_ms);
#endif
