#ifndef GW_TIME_H
#define GW_TIME_H

#include <stdbool.h>
#include <stdint.h>

/* Monotonic milliseconds since the FreeRTOS scheduler tick started. */
uint64_t gw_time_ms(void);

/* UTC milliseconds since Unix epoch. Before SNTP synchronization this returns
 * 0 so callers never confuse uptime with wall-clock time. */
uint64_t gw_time_utc_ms(void);
/* Timestamp for persisted/telemetry data: UTC after synchronization, otherwise monotonic uptime. */
uint64_t gw_time_data_ms(void);
bool gw_time_is_synchronized(void);
uint64_t gw_time_last_sync_monotonic_ms(void);
void gw_time_set_utc_seconds(uint32_t unix_seconds);
void gw_time_set_utc_ms(uint64_t unix_ms);

/* Called exactly once per RTOS tick from vApplicationTickHook(). */
void gw_time_tick_from_isr(void);
/* ISR/exception-safe 32-bit monotonic counter for FreeRTOS run-time stats.
 * Must not call FreeRTOS task-context APIs because the kernel invokes it from
 * PendSV during context switches. Resolution is one RTOS tick (1 ms). */
uint32_t gw_time_runtime_counter32(void);

#endif
