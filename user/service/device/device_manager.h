#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include "gw_error.h"
#include "gw_types.h"

void device_manager_init(void);
gw_err_t device_manager_register(const gw_device_t *device);
gw_err_t device_manager_upsert(const gw_device_t *device);
gw_err_t device_manager_remove(uint32_t id);
void device_manager_reset(void);
gw_err_t device_manager_get(uint32_t id, gw_device_t *out);
/* Find a configured device by its southbound binding. This is used by
 * ad-hoc operator commands so a Slave ID can transparently attach to the
 * persistent device model when one exists. */
gw_err_t device_manager_find_binding(gw_protocol_t protocol,
                                     gw_interface_id_t interface_id,
                                     uint16_t address,
                                     gw_device_t *out);
uint32_t device_manager_count(void);
uint32_t device_manager_snapshot(gw_device_t *out, uint32_t max_count);
void device_manager_report_success(uint32_t id, uint64_t now_ms);
void device_manager_report_failure(uint32_t id, gw_err_t reason, uint64_t now_ms);

#endif
