#include "gw_syslog.h"
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "gateway_build_config.h"
#include "gw_config.h"
#include "gw_time.h"
#include "rtos_objects.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"

#if ((GW_ETH_ENABLE!=0U)&&(GW_SYSLOG_ENABLE!=0U))
#define SYSLOG_Q_LEN 16U
#define SYSLOG_LINE 192U
#define SYSLOG_STACK_WORDS 768U
#define SYSLOG_PRIORITY 1U
typedef struct{uint16_t len;uint8_t severity;char data[SYSLOG_LINE];}syslog_item_t;
static StaticQueue_t s_qcb;static uint8_t s_qstorage[SYSLOG_Q_LEN*sizeof(syslog_item_t)];static QueueHandle_t s_q;static StaticTask_t s_tcb;static StackType_t s_stack[SYSLOG_STACK_WORDS];static gw_syslog_stats_t s_stats;
void gw_syslog_init(void){memset(&s_stats,0,sizeof(s_stats));s_q=xQueueCreateStatic(SYSLOG_Q_LEN,sizeof(syslog_item_t),s_qstorage,&s_qcb);configASSERT(s_q!=NULL);}
void gw_syslog_capture(const char *level,const char *tag,const char *line,size_t length){if((s_q==NULL)||(level==NULL)||(tag==NULL)||(line==NULL))return;if((level[0]!='E')&&(level[0]!='W'))return;syslog_item_t i;memset(&i,0,sizeof(i));int pri=(level[0]=='E')?11:12;uint64_t utc=gw_time_utc_ms();i.severity=(level[0]=='E')?0U:1U;int n=snprintf(i.data,sizeof(i.data),"<%d>1 %llu GW-H759 gateway - - - [%s] %s",pri,(unsigned long long)utc,tag,line);if(n<0)return;i.len=(uint16_t)(((size_t)n<sizeof(i.data))?(size_t)n:sizeof(i.data)-1U);(void)length;if(xQueueSend(s_q,&i,0U)==pdTRUE)++s_stats.queued;else ++s_stats.dropped;}
static void syslog_task(void *arg){(void)arg;struct netconn *c=NULL;syslog_item_t item;for(;;){if(xQueueReceive(s_q,&item,pdMS_TO_TICKS(500U))!=pdTRUE)continue;if((xEventGroupGetBits(g_system_events)&EVT_NET_IP_READY)==0U){++s_stats.dropped;continue;}gw_runtime_config_t cfg;gw_config_get_runtime(&cfg);if((cfg.syslog_min_level==0U)&&(item.severity!=0U))continue;ip_addr_t addr;if(!ipaddr_aton(cfg.syslog_server,&addr)){++s_stats.send_errors;continue;}if(c==NULL){c=netconn_new(NETCONN_UDP);if(c==NULL){++s_stats.send_errors;continue;}}if(netconn_connect(c,&addr,cfg.syslog_port)!=ERR_OK){++s_stats.send_errors;continue;}struct netbuf*b=netbuf_new();if(b==NULL){++s_stats.send_errors;continue;}void*p=netbuf_alloc(b,item.len);if(p==NULL){netbuf_delete(b);++s_stats.send_errors;continue;}memcpy(p,item.data,item.len);err_t e=netconn_send(c,b);netbuf_delete(b);if(e==ERR_OK)++s_stats.sent;else ++s_stats.send_errors;}}
void gw_syslog_task_create(void){TaskHandle_t h=xTaskCreateStatic(syslog_task,"syslog",SYSLOG_STACK_WORDS,NULL,SYSLOG_PRIORITY,s_stack,&s_tcb);configASSERT(h!=NULL);}
void gw_syslog_get_stats(gw_syslog_stats_t*out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_stats;taskEXIT_CRITICAL();}
#else
void gw_syslog_init(void){}void gw_syslog_task_create(void){}void gw_syslog_capture(const char*l,const char*t,const char*x,size_t n){(void)l;(void)t;(void)x;(void)n;}void gw_syslog_get_stats(gw_syslog_stats_t*o){if(o)memset(o,0,sizeof(*o));}
#endif
