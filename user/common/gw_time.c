#include "gw_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include "rtos_objects.h"

static volatile uint32_t s_tick_low;
static volatile uint32_t s_tick_high;
static uint64_t s_utc_base_ms;
static uint64_t s_sync_monotonic_ms;
static bool s_synced;

void gw_time_tick_from_isr(void)
{
    uint32_t low = s_tick_low + 1U;
    s_tick_low = low;
    if (low == 0U) ++s_tick_high;
}

uint32_t gw_time_runtime_counter32(void)
{
    /* 32-bit aligned loads are atomic on Cortex-M7.  Do not enter a FreeRTOS
     * critical section here: configGENERATE_RUN_TIME_STATS calls this function
     * from vTaskSwitchContext(), which executes inside PendSV.  Calling
     * taskENTER_CRITICAL() there trips the CM7 port ISR-context assert and
     * freezes scheduling before the second task can run. */
    return s_tick_low;
}

uint64_t gw_time_ms(void)
{
    uint32_t high, low;
    taskENTER_CRITICAL(); high=s_tick_high; low=s_tick_low; taskEXIT_CRITICAL();
    uint64_t ticks=((uint64_t)high<<32U)|(uint64_t)low;
    return (ticks*1000ULL)/(uint64_t)configTICK_RATE_HZ;
}

void gw_time_set_utc_ms(uint64_t unix_ms)
{
    uint64_t mono=gw_time_ms();
    taskENTER_CRITICAL();
    s_utc_base_ms=unix_ms;
    s_sync_monotonic_ms=mono;
    s_synced=true;
    taskEXIT_CRITICAL();
    if (g_system_events != NULL) (void)xEventGroupSetBits(g_system_events, EVT_TIME_SYNCED);
}

void gw_time_set_utc_seconds(uint32_t unix_seconds)
{
    gw_time_set_utc_ms((uint64_t)unix_seconds*1000ULL);
}

uint64_t gw_time_utc_ms(void)
{
    uint64_t base,sync;bool ok;uint64_t mono=gw_time_ms();
    taskENTER_CRITICAL();base=s_utc_base_ms;sync=s_sync_monotonic_ms;ok=s_synced;taskEXIT_CRITICAL();
    if(!ok)return 0U;
    return base + ((mono>=sync)?(mono-sync):0U);
}

uint64_t gw_time_data_ms(void)
{
    uint64_t utc=gw_time_utc_ms();
    return (utc!=0U)?utc:gw_time_ms();
}

bool gw_time_is_synchronized(void){bool v;taskENTER_CRITICAL();v=s_synced;taskEXIT_CRITICAL();return v;}
uint64_t gw_time_last_sync_monotonic_ms(void){uint64_t v;taskENTER_CRITICAL();v=s_sync_monotonic_ms;taskEXIT_CRITICAL();return v;}
