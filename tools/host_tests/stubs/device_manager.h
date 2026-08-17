#ifndef HOST_DEVICE_MANAGER_H
#define HOST_DEVICE_MANAGER_H
#include "gw_error.h"
#include "gw_types.h"
gw_err_t device_manager_get(uint32_t id, gw_device_t *out);
void device_manager_report_success(uint32_t id, uint64_t now_ms);
void device_manager_report_failure(uint32_t id, gw_err_t reason, uint64_t now_ms);
#endif
