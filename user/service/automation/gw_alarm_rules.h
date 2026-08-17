#ifndef GW_ALARM_RULES_H
#define GW_ALARM_RULES_H

#include <stdbool.h>
#include <stdint.h>
#include "gw_error.h"
#include "gw_types.h"

typedef enum {
    GW_ALARM_HIGH = 0,
    GW_ALARM_LOW,
    GW_ALARM_RATE_HIGH,
} gw_alarm_kind_t;

typedef struct {
    uint32_t id;
    uint32_t point_id;
    gw_alarm_kind_t kind;
    double threshold;
    double hysteresis;
    uint8_t priority;
    bool enabled;
} gw_alarm_rule_t;

typedef enum {
    GW_RULE_OP_GT = 0,
    GW_RULE_OP_GE,
    GW_RULE_OP_LT,
    GW_RULE_OP_LE,
} gw_rule_operator_t;

typedef enum {
    GW_RULE_ACTION_CAN = 0,
    GW_RULE_ACTION_MODBUS_FC06,
} gw_rule_action_t;

typedef struct {
    uint32_t id;
    uint32_t point_id;
    gw_rule_operator_t op;
    double threshold;
    double hysteresis;
    uint32_t require_device_id; /* 0 = no online-state prerequisite */
    gw_rule_action_t action;
    uint32_t cooldown_ms;
    bool enabled;

    /* CAN action. Production invariant still forces BRS off. */
    uint32_t can_id;
    bool can_extended;
    bool can_fd;
    uint8_t can_len;
    uint8_t can_data[64];

    /* Modbus FC06 action. */
    uint8_t modbus_slave;
    uint16_t modbus_register;
    uint16_t modbus_value;
} gw_linkage_rule_t;

typedef struct {
    uint32_t alarm_raised;
    uint32_t alarm_cleared;
    uint32_t alarm_eval_error;
    uint32_t rule_triggered;
    uint32_t rule_suppressed;
    uint32_t rule_action_error;
} gw_automation_stats_t;

void gw_automation_init(void);
gw_err_t gw_alarm_upsert(const gw_alarm_rule_t *rule);
gw_err_t gw_alarm_remove(uint32_t id);
uint32_t gw_alarm_snapshot(gw_alarm_rule_t *out, uint32_t max_count);

gw_err_t gw_linkage_upsert(const gw_linkage_rule_t *rule);
gw_err_t gw_linkage_remove(uint32_t id);
uint32_t gw_linkage_snapshot(gw_linkage_rule_t *out, uint32_t max_count);
void gw_automation_reset(void);

/* Called after a Point DB update has committed. */
void gw_automation_on_point(uint32_t point_id);
void gw_automation_get_stats(gw_automation_stats_t *out);

#endif
