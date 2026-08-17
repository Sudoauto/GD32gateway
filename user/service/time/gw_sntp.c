#include "gw_sntp.h"

#include <string.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"
#include "gateway_build_config.h"
#include "gw_log.h"
#include "gw_time.h"
#include "rtos_objects.h"
#include "lwip/apps/sntp.h"
#include "lwip/tcpip.h"

#if ((GW_ETH_ENABLE != 0U) && (GW_SNTP_ENABLE != 0U))
#define SNTP_TASK_STACK_WORDS 512U
#define SNTP_TASK_PRIORITY    1U

static StaticTask_t s_tcb;
static StackType_t s_stack[SNTP_TASK_STACK_WORDS];
static char s_server[2][64];
static uint8_t s_active_server;
static bool s_running;
static uint32_t s_restart_count;

static void sntp_start_cb(void *ctx)
{
    uint8_t slot=(ctx!=NULL)?(uint8_t)(uintptr_t)ctx:s_active_server;
    if(slot>1U)slot=s_active_server;
    if (sntp_enabled()) sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0U, s_server[slot]);
    s_active_server=slot;
    sntp_init();
    s_running=true;
    ++s_restart_count;
}

static void sntp_stop_cb(void *ctx)
{
    (void)ctx;
    if (sntp_enabled()) sntp_stop();
    s_running=false;
}

void gw_sntp_init(void)
{
    memset(s_server,0,sizeof(s_server));
    strncpy(s_server[0],GW_SNTP_SERVER,sizeof(s_server[0])-1U);
    strncpy(s_server[1],GW_SNTP_SERVER,sizeof(s_server[1])-1U);
    s_active_server=0U;s_running=false;s_restart_count=0U;
}

void gw_sntp_set_server(const char *server)
{
    if ((server==NULL)||(server[0]=='\0')) return;
    uint8_t slot;
    taskENTER_CRITICAL();
    slot=(uint8_t)(s_active_server^1U);
    strncpy(s_server[slot],server,sizeof(s_server[slot])-1U);s_server[slot][sizeof(s_server[slot])-1U]='\0';
    taskEXIT_CRITICAL();
    if ((g_system_events!=NULL)&&((xEventGroupGetBits(g_system_events)&EVT_NET_IP_READY)!=0U))
        (void)tcpip_callback(sntp_start_cb,(void *)(uintptr_t)slot);
    else s_active_server=slot;
}

static void sntp_task(void *arg)
{
    (void)arg;
    bool was_ready=false;
    for(;;){
        EventBits_t bits=xEventGroupGetBits(g_system_events);
        bool ready=(bits&EVT_NET_IP_READY)!=0U;
        if(ready&&!was_ready){
            (void)tcpip_callback(sntp_start_cb,NULL);
            GW_LOGI("TIME","SNTP start server=%s",s_server[s_active_server]);
        }else if(!ready&&was_ready){
            (void)tcpip_callback(sntp_stop_cb,NULL);
        }
        if(gw_time_is_synchronized() && ((bits&EVT_TIME_SYNCED)!=0U)){
            static bool logged=false;
            if(!logged){GW_LOGI("TIME","UTC synchronized unix_ms=%llu",(unsigned long long)gw_time_utc_ms());logged=true;}
        }
        was_ready=ready;
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

void gw_sntp_task_create(void)
{
    TaskHandle_t h=xTaskCreateStatic(sntp_task,"sntp",SNTP_TASK_STACK_WORDS,NULL,SNTP_TASK_PRIORITY,s_stack,&s_tcb);
    configASSERT(h!=NULL);
}

bool gw_sntp_running(void){return s_running;}
uint32_t gw_sntp_restart_count(void){return s_restart_count;}

#else
void gw_sntp_init(void){}
void gw_sntp_task_create(void){}
void gw_sntp_set_server(const char *server){(void)server;}
bool gw_sntp_running(void){return false;}
uint32_t gw_sntp_restart_count(void){return 0U;}
#endif
