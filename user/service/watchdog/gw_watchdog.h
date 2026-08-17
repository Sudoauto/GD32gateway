#ifndef GW_WATCHDOG_H
#define GW_WATCHDOG_H

#include <stdint.h>

typedef enum {
    GW_WD_CONFIG=0,
    GW_WD_RS485,
    GW_WD_DATA,
    GW_WD_CAN,
    GW_WD_POLL,
    GW_WD_NET,
    GW_WD_UPLINK,
    GW_WD_GUI,
    GW_WD_COUNT
} gw_watchdog_channel_t;

typedef struct {uint32_t required_mask;uint32_t stale_mask;uint32_t feed_count;uint32_t unhealthy_count;uint64_t last_beat_ms[GW_WD_COUNT];} gw_watchdog_stats_t;

void gw_watchdog_init(void);
void gw_watchdog_task_create(void);
void gw_watchdog_beat(gw_watchdog_channel_t channel);
void gw_watchdog_get_stats(gw_watchdog_stats_t *out);

#endif
