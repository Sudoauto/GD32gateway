#include "gw_watchdog.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "gd32h7xx_fwdgt.h"
#include "gateway_build_config.h"
#include "gw_log.h"
#include "gw_time.h"

#if (GW_WATCHDOG_ENABLE!=0U)
#define WD_STACK_WORDS 512U
#define WD_PRIORITY    6U
static StaticTask_t s_tcb;static StackType_t s_stack[WD_STACK_WORDS];static gw_watchdog_stats_t s_stats;static uint64_t s_start_ms;

static uint32_t required_mask(void){uint32_t m=(1UL<<GW_WD_CONFIG)|(1UL<<GW_WD_RS485)|(1UL<<GW_WD_DATA)|(1UL<<GW_WD_POLL);
#if (GW_CANFD_ENABLE!=0)
m|=(1UL<<GW_WD_CAN);
#endif
#if (GW_ETH_ENABLE!=0U)
m|=(1UL<<GW_WD_NET);
#endif
#if ((GW_ETH_ENABLE!=0U)&&(GW_UPLINK_ENABLE!=0U))
m|=(1UL<<GW_WD_UPLINK);
#endif
#if (GW_GUI_ENABLE!=0U)
m|=(1UL<<GW_WD_GUI);
#endif
return m;}

void gw_watchdog_init(void){memset(&s_stats,0,sizeof(s_stats));s_stats.required_mask=required_mask();s_start_ms=gw_time_ms();}
void gw_watchdog_beat(gw_watchdog_channel_t c){if(c>=GW_WD_COUNT)return;taskENTER_CRITICAL();s_stats.last_beat_ms[c]=gw_time_ms();taskEXIT_CRITICAL();}

static void hw_start(void)
{
    /* IRC32K / 256 ~= 125 Hz. Reload is rounded upward and clamped to the
     * 12-bit hardware counter. */
    uint32_t reload=(GW_WATCHDOG_TIMEOUT_MS*125U+999U)/1000U;if(reload<2U)reload=2U;if(reload>4095U)reload=4095U;
    (void)fwdgt_config((uint16_t)reload,FWDGT_PSC_DIV256);fwdgt_counter_reload();fwdgt_enable();
}
static void wd_task(void *arg)
{
    (void)arg;
    hw_start();
    GW_LOGI("WD", "supervisor started timeout=%ums grace=%ums mask=0x%08lX",
            (unsigned)GW_WATCHDOG_TIMEOUT_MS,
            (unsigned)GW_WATCHDOG_START_GRACE_MS,
            (unsigned long)s_stats.required_mask);
    bool warned=false;for(;;){uint64_t now=gw_time_ms();uint32_t stale=0U;uint32_t req=s_stats.required_mask;
        if(now-s_start_ms>=GW_WATCHDOG_START_GRACE_MS){for(uint32_t i=0U;i<GW_WD_COUNT;++i)if((req&(1UL<<i))!=0U){uint64_t b=s_stats.last_beat_ms[i];if((b==0U)||(now-b>GW_WATCHDOG_STALE_MS))stale|=(1UL<<i);}}
        s_stats.stale_mask=stale;if(stale==0U){fwdgt_counter_reload();++s_stats.feed_count;warned=false;}else{++s_stats.unhealthy_count;if(!warned){GW_LOGE("WD","supervisor stale mask=0x%08lX; FWDGT intentionally not fed",(unsigned long)stale);warned=true;}}
        vTaskDelay(pdMS_TO_TICKS(GW_WATCHDOG_TASK_PERIOD_MS));}
}
void gw_watchdog_task_create(void){TaskHandle_t h=xTaskCreateStatic(wd_task,"watchdog",WD_STACK_WORDS,NULL,WD_PRIORITY,s_stack,&s_tcb);configASSERT(h!=NULL);}
void gw_watchdog_get_stats(gw_watchdog_stats_t *out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_stats;taskEXIT_CRITICAL();}
#else
void gw_watchdog_init(void){}void gw_watchdog_task_create(void){}void gw_watchdog_beat(gw_watchdog_channel_t c){(void)c;}void gw_watchdog_get_stats(gw_watchdog_stats_t *o){if(o)memset(o,0,sizeof(*o));}
#endif
