#ifndef GW_UPLINK_H
#define GW_UPLINK_H

#include <stdbool.h>
#include <stdint.h>
#include "drv_canfd.h"
#include "gw_error.h"
#include "gw_types.h"

#define GW_UPLINK_RAW_MAX       256U
#define GW_UPLINK_HISTORY_DEPTH 12U

typedef struct {
    uint32_t sequence;
    gw_interface_id_t interface_id;
    gw_protocol_t protocol;
    bool tx_direction;
    uint32_t device_id;
    uint64_t timestamp_ms;
    uint32_t address;
    uint16_t code;
    uint16_t flags;
    gw_err_t result;
    uint16_t length;
    uint8_t data[GW_UPLINK_RAW_MAX];
} gw_uplink_event_t;

typedef struct {
    bool task_started;
    bool listening;
    bool client_connected;
    uint16_t listen_port;
    uint32_t accept_count;
    uint32_t disconnect_count;
    uint32_t frame_queued;
    uint32_t frame_dropped;
    uint32_t frame_sent;
    uint32_t point_sent;
    uint32_t snapshot_sent;
    uint32_t send_error_count;
    uint32_t rx_command_count;
    uint32_t command_error_count;
    uint32_t tx_bytes;
    uint32_t history_count;
    uint32_t alarm_sent;
    uint32_t offline_spooled;
    uint32_t offline_replayed;
    uint32_t auth_fail;
} gw_uplink_stats_t;

void gw_uplink_init(void);
void gw_uplink_task_create(void);
void gw_uplink_get_stats(gw_uplink_stats_t *out);

/* Common southbound event ingress. New protocol/interface adapters should fill
 * this envelope instead of inventing a new northbound representation. */
void gw_uplink_publish_event(const gw_uplink_event_t *event);

void gw_uplink_publish_can(const canfd_frame_t *frame, bool tx_direction);
void gw_uplink_publish_point_record(uint32_t point_id);
void gw_uplink_publish_alarm(uint32_t alarm_id, uint32_t point_id, uint32_t alarm_kind,
                             bool active, uint8_t priority, double value, double threshold);
void gw_uplink_publish_rule_action(uint32_t rule_id, uint32_t point_id, gw_err_t result);

void gw_uplink_publish_modbus(const uint8_t *frame, uint16_t length,
                              uint32_t device_id, uint8_t slave,
                              bool tx_direction, gw_err_t result);

/* Oldest-to-newest snapshot of recent southbound frames for the local HMI. */
uint32_t gw_uplink_history_snapshot(gw_uplink_event_t *out, uint32_t max_count);

#endif /* GW_UPLINK_H */
