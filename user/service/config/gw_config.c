#include "gw_config.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "semphr.h"
#include "task.h"
#include "gd32h7xx.h"
#include "can_decoder.h"
#include "device_manager.h"
#include "drv_rs485.h"
#include "gateway_build_config.h"
#include "gw_alarm_rules.h"
#include "gw_flash_store.h"
#include "gw_log.h"
#include "gw_sntp.h"
#include "gw_time.h"
#include "gw_types.h"
#include "gw_watchdog.h"
#include "point_db.h"
#include "poll_scheduler.h"
#include "rtos_objects.h"

#if (GW_RUNTIME_CONFIG_ENABLE != 0U)
#define CONFIG_TASK_STACK_WORDS 1024U
#define CONFIG_TASK_PRIORITY    1U
#define CSV_LINE_MAX            640U

static StaticTask_t s_tcb;
static StackType_t s_stack[CONFIG_TASK_STACK_WORDS];
static gw_runtime_config_t s_runtime;
static gw_config_stats_t s_stats;
static char s_csv[GW_CONFIG_MAX_CSV_BYTES + 1U];
static char s_rollback_csv[GW_CONFIG_MAX_CSV_BYTES + 1U];
static uint64_t s_dirty_since;
static bool s_save_requested;
static bool s_factory_reset_requested;
static bool s_factory_reset_clear_spool;

static void runtime_store(const gw_runtime_config_t *cfg)
{
    if(cfg==NULL)return;
    taskENTER_CRITICAL();
    s_runtime=*cfg;
    taskEXIT_CRITICAL();
}

static void runtime_load(gw_runtime_config_t *cfg)
{
    if(cfg==NULL)return;
    taskENTER_CRITICAL();
    *cfg=s_runtime;
    taskEXIT_CRITICAL();
}

static void defaults(void)
{
    gw_runtime_config_t r;
    memset(&r,0,sizeof(r));
    r.dhcp=(GW_ETH_DHCP_ENABLE!=0U);
    r.ip[0]=GW_ETH_IP0;r.ip[1]=GW_ETH_IP1;r.ip[2]=GW_ETH_IP2;r.ip[3]=GW_ETH_IP3;
    r.mask[0]=GW_ETH_MASK0;r.mask[1]=GW_ETH_MASK1;r.mask[2]=GW_ETH_MASK2;r.mask[3]=GW_ETH_MASK3;
    r.gateway[0]=GW_ETH_GW0;r.gateway[1]=GW_ETH_GW1;r.gateway[2]=GW_ETH_GW2;r.gateway[3]=GW_ETH_GW3;
    r.rs485_baudrate=GW_RS485_BAUDRATE;r.rs485_data_bits=GW_RS485_DATA_BITS;r.rs485_stop_bits=GW_RS485_STOP_BITS;r.rs485_parity=(uint8_t)GW_RS485_PARITY;r.rs485_timeout_ms=GW_RS485_RESPONSE_TIMEOUT_MS;r.rs485_retry=GW_RS485_RETRY_COUNT;
    strncpy(r.sntp_server,GW_SNTP_SERVER,sizeof(r.sntp_server)-1U);
    strncpy(r.syslog_server,GW_SYSLOG_DEFAULT_SERVER,sizeof(r.syslog_server)-1U);r.syslog_port=GW_SYSLOG_DEFAULT_PORT;r.syslog_min_level=1U;
    strncpy(r.snmp_community,GW_SNMP_COMMUNITY,sizeof(r.snmp_community)-1U);
    runtime_store(&r);
}

static unsigned long ul(const char *s){return (s!=NULL)?strtoul(s,NULL,0):0UL;}
static double dbl(const char *s){return (s!=NULL)?strtod(s,NULL):0.0;}
static bool boolean(const char *s){return (s!=NULL)&&((strcmp(s,"1")==0)||(strcmp(s,"true")==0)||(strcmp(s,"on")==0));}
static void safe_copy(char *dst,size_t n,const char *src){if((dst==NULL)||(n==0U))return;if(src==NULL){dst[0]='\0';return;}strncpy(dst,src,n-1U);dst[n-1U]='\0';}

static uint32_t split_csv(char *line,char **f,uint32_t max)
{
    uint32_t n=0U;char *p=line;while((p!=NULL)&&(n<max)){f[n++]=p;char *comma=strchr(p,',');if(comma==NULL)break;*comma='\0';p=comma+1;}return n;
}

static gw_err_t apply_net(char **f,uint32_t n)
{
    if(n<14U)return GW_ERR_PARAM;
    gw_runtime_config_t r;runtime_load(&r);
    r.dhcp=boolean(f[1]);
    for(uint32_t i=0U;i<4U;++i){unsigned long v=ul(f[2U+i]);if(v>255U)return GW_ERR_PARAM;r.ip[i]=(uint8_t)v;}
    for(uint32_t i=0U;i<4U;++i){unsigned long v=ul(f[6U+i]);if(v>255U)return GW_ERR_PARAM;r.mask[i]=(uint8_t)v;}
    for(uint32_t i=0U;i<4U;++i){unsigned long v=ul(f[10U+i]);if(v>255U)return GW_ERR_PARAM;r.gateway[i]=(uint8_t)v;}
    runtime_store(&r);
    return GW_OK;
}
static gw_err_t apply_rs485(char **f,uint32_t n){if(n<7U)return GW_ERR_PARAM;uint32_t baud=(uint32_t)ul(f[1]);uint8_t db=(uint8_t)ul(f[2]),par=(uint8_t)ul(f[3]),stop=(uint8_t)ul(f[4]);uint32_t to=(uint32_t)ul(f[5]);uint8_t retry=(uint8_t)ul(f[6]);if((baud==0U)||(db!=8U)||((stop!=1U)&&(stop!=2U))||(par>2U)||(to==0U))return GW_ERR_PARAM;gw_runtime_config_t r;runtime_load(&r);r.rs485_baudrate=baud;r.rs485_data_bits=db;r.rs485_parity=par;r.rs485_stop_bits=stop;r.rs485_timeout_ms=to;r.rs485_retry=retry;runtime_store(&r);return GW_OK;}
static gw_err_t apply_services(char **f,uint32_t n){if(n<7U)return GW_ERR_PARAM;uint32_t port=(uint32_t)ul(f[3]);uint32_t level=(uint32_t)ul(f[4]);if((port==0U)||(port>65535U)||(level>1U)||(strlen(f[5])>=sizeof(((gw_runtime_config_t*)0)->snmp_community)))return GW_ERR_PARAM;gw_runtime_config_t r;runtime_load(&r);safe_copy(r.sntp_server,sizeof(r.sntp_server),f[1]);safe_copy(r.syslog_server,sizeof(r.syslog_server),f[2]);r.syslog_port=(uint16_t)port;r.syslog_min_level=(uint8_t)level;safe_copy(r.snmp_community,sizeof(r.snmp_community),f[5]);(void)f[6];runtime_store(&r);return GW_OK;}

static gw_err_t apply_device(char **f,uint32_t n){if(n<9U)return GW_ERR_PARAM;gw_device_t d;memset(&d,0,sizeof(d));d.id=(uint32_t)ul(f[1]);safe_copy(d.name,sizeof(d.name),f[2]);d.protocol=(gw_protocol_t)ul(f[3]);d.interface_id=(gw_interface_id_t)ul(f[4]);d.address=(uint16_t)ul(f[5]);d.timeout_ms=(uint32_t)ul(f[6]);d.retry=(uint8_t)ul(f[7]);d.state=boolean(f[8])?DEVICE_INIT:DEVICE_DISABLED;d.valid=true;return device_manager_upsert(&d);}
static gw_err_t apply_point(char **f,uint32_t n){if(n<7U)return GW_ERR_PARAM;gw_point_t p;memset(&p,0,sizeof(p));p.id=(uint32_t)ul(f[1]);p.device_id=(uint32_t)ul(f[2]);safe_copy(p.name,sizeof(p.name),f[3]);p.type=(gw_value_type_t)ul(f[4]);p.scale=(float)dbl(f[5]);p.offset=(float)dbl(f[6]);p.quality=GW_QUALITY_STALE;p.valid=true;return point_db_upsert(&p);}
static gw_err_t apply_canmap(char **f,uint32_t n){if(n<11U)return GW_ERR_PARAM;can_signal_map_t m;memset(&m,0,sizeof(m));m.id=(uint32_t)ul(f[1]);m.device_id=(uint32_t)ul(f[2]);m.point_id=(uint32_t)ul(f[3]);m.can_id=(uint32_t)ul(f[4]);m.byte_offset=(uint8_t)ul(f[5]);m.encoding=(can_signal_encoding_t)ul(f[6]);m.endian=(can_endian_t)ul(f[7]);m.extended=boolean(f[8]);m.require_fd=boolean(f[9]);m.enabled=boolean(f[10]);return can_decoder_upsert(&m);}
static gw_err_t apply_poll(char **f,uint32_t n){if(n<11U)return GW_ERR_PARAM;poll_job_t j;memset(&j,0,sizeof(j));j.id=(uint32_t)ul(f[1]);j.device_id=(uint32_t)ul(f[2]);j.point_id=(uint32_t)ul(f[3]);j.function_code=(uint8_t)ul(f[4]);j.start_address=(uint16_t)ul(f[5]);j.quantity=(uint16_t)ul(f[6]);j.register_offset=(uint16_t)ul(f[7]);j.interval_ms=(uint32_t)ul(f[8]);j.encoding=(poll_encoding_t)ul(f[9]);j.enabled=boolean(f[10]);return poll_scheduler_upsert(&j);}
static gw_err_t apply_alarm(char **f,uint32_t n){if(n<8U)return GW_ERR_PARAM;gw_alarm_rule_t r;memset(&r,0,sizeof(r));r.id=(uint32_t)ul(f[1]);r.point_id=(uint32_t)ul(f[2]);r.kind=(gw_alarm_kind_t)ul(f[3]);r.threshold=dbl(f[4]);r.hysteresis=dbl(f[5]);r.priority=(uint8_t)ul(f[6]);r.enabled=boolean(f[7]);return gw_alarm_upsert(&r);}
static gw_err_t parse_hex(const char *s,uint8_t *out,uint8_t cap,uint8_t *len){uint8_t n=0U;if((s==NULL)||(out==NULL)||(len==NULL))return GW_ERR_PARAM;while((*s!='\0')&&(n<cap)){char t[3]={0,0,0};if(s[1]=='\0')return GW_ERR_PARAM;t[0]=s[0];t[1]=s[1];char *end=NULL;unsigned long v=strtoul(t,&end,16);if((end==t)||(*end!='\0')||(v>255U))return GW_ERR_PARAM;out[n++]=(uint8_t)v;s+=2;}*len=n;return GW_OK;}
static gw_err_t apply_rule(char **f,uint32_t n){if(n<16U)return GW_ERR_PARAM;gw_linkage_rule_t r;memset(&r,0,sizeof(r));r.id=(uint32_t)ul(f[1]);r.point_id=(uint32_t)ul(f[2]);r.op=(gw_rule_operator_t)ul(f[3]);r.threshold=dbl(f[4]);r.hysteresis=dbl(f[5]);r.require_device_id=(uint32_t)ul(f[6]);r.action=(gw_rule_action_t)ul(f[7]);r.cooldown_ms=(uint32_t)ul(f[8]);r.can_id=(uint32_t)ul(f[9]);r.can_extended=boolean(f[10]);r.can_fd=boolean(f[11]);if(parse_hex(f[12],r.can_data,sizeof(r.can_data),&r.can_len)!=GW_OK)return GW_ERR_PARAM;r.modbus_slave=(uint8_t)ul(f[13]);r.modbus_register=(uint16_t)ul(f[14]);r.modbus_value=(uint16_t)ul(f[15]);r.enabled=(n>16U)?boolean(f[16]):true;return gw_linkage_upsert(&r);}
static int hexn(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static bool hex_decode_fixed(const char *s,uint8_t *out,size_t n){if(s==NULL)return false;for(size_t i=0;i<n;++i){int a=hexn(s[i*2]),b=hexn(s[i*2+1]);if(a<0||b<0)return false;out[i]=(uint8_t)((a<<4)|b);}return s[n*2]=='\0';}
static gw_err_t apply_auth(char **f,uint32_t n){if(n<4U)return GW_ERR_PARAM;gw_security_credential_t c;memset(&c,0,sizeof(c));safe_copy(c.username,sizeof(c.username),f[1]);if(!hex_decode_fixed(f[2],c.salt,sizeof(c.salt))||!hex_decode_fixed(f[3],c.password_hash,sizeof(c.password_hash)))return GW_ERR_PARAM;return gw_security_set_credential(&c);}

static gw_err_t apply_line_mutable(char *line)
{
    while((*line==' ')||(*line=='\t'))++line;if((*line=='\0')||(*line=='#'))return GW_OK;
    size_t l=strlen(line);while(l>0U&&((line[l-1]=='\r')||(line[l-1]=='\n')||(line[l-1]==' ')||(line[l-1]=='\t')))line[--l]='\0';
    char *f[20];uint32_t n=split_csv(line,f,20U);if(n==0U)return GW_ERR_PARAM;
    if(strcmp(f[0],"NET")==0)return apply_net(f,n);if(strcmp(f[0],"RS485")==0)return apply_rs485(f,n);if(strcmp(f[0],"SERVICE")==0)return apply_services(f,n);if(strcmp(f[0],"AUTH")==0)return apply_auth(f,n);
    if(strcmp(f[0],"DEVICE")==0)return apply_device(f,n);if(strcmp(f[0],"POINT")==0)return apply_point(f,n);if(strcmp(f[0],"CANMAP")==0)return apply_canmap(f,n);if(strcmp(f[0],"POLL")==0)return apply_poll(f,n);if(strcmp(f[0],"ALARM")==0)return apply_alarm(f,n);if(strcmp(f[0],"RULE")==0)return apply_rule(f,n);
    return GW_ERR_NOT_SUPPORTED;
}

static void dynamic_reset(void){can_decoder_reset();poll_scheduler_reset();gw_automation_reset();point_db_reset();device_manager_reset();}

static gw_err_t apply_document(char *csv,size_t length,bool count_errors)
{
    if((csv==NULL)||(length==0U)||(length>GW_CONFIG_MAX_CSV_BYTES))return GW_ERR_PARAM;
    dynamic_reset();defaults();gw_security_init();
    gw_err_t first_error=GW_OK;
    char *line=csv;
    while(line!=NULL && *line!='\0'){
        char *next=strchr(line,'\n');
        if(next!=NULL){*next='\0';++next;}
        size_t ll=strlen(line);
        if((ll!=0U)&&(line[ll-1U]=='\r')) line[ll-1U]='\0';
        char temp[CSV_LINE_MAX];safe_copy(temp,sizeof(temp),line);
        gw_err_t e=apply_line_mutable(temp);
        if((e!=GW_OK)&&(first_error==GW_OK)){first_error=e;}
        if((e!=GW_OK)&&count_errors){++s_stats.rejected_row_count;++s_stats.parse_error_count;}
        line=(next!=NULL && *next!='\0')?next:NULL;
    }
    gw_runtime_config_t r;runtime_load(&r);gw_sntp_set_server(r.sntp_server);
    return first_error;
}

static gw_err_t import_locked(char *csv,size_t length)
{
    if((csv==NULL)||(length==0U)||(length>GW_CONFIG_MAX_CSV_BYTES))return GW_ERR_PARAM;
    /* Full-document replacement is transactional: capture the current model as
     * canonical CSV, try the new document, and restore the snapshot on any
     * rejected row. Remote single-row CFGSET remains an intentional upsert. */
    size_t rollback_len=gw_config_export_csv(s_rollback_csv,sizeof(s_rollback_csv));
    if((rollback_len==0U)||(rollback_len>=sizeof(s_rollback_csv)))return GW_ERR_FULL;
    s_rollback_csv[rollback_len]='\0';
    (void)xEventGroupSetBits(g_system_events,EVT_CONFIG_UPDATING);
    gw_err_t e=apply_document(csv,length,true);
    if(e!=GW_OK){
        /* apply_document tokenizes in place; rollback uses its own buffer. */
        gw_err_t restore=apply_document(s_rollback_csv,rollback_len,false);
        if(restore!=GW_OK){
            defaults();dynamic_reset();gw_security_init();
            e=GW_ERR_STATE;
        }
    }
    (void)xEventGroupClearBits(g_system_events,EVT_CONFIG_UPDATING);
    return e;
}

static void append(char *out,size_t cap,size_t *used,const char *fmt,...)
{
    if(*used>=cap)return;va_list ap;va_start(ap,fmt);int n=vsnprintf(out+*used,cap-*used,fmt,ap);va_end(ap);if(n>0){size_t add=(size_t)n;if(add>cap-*used)add=cap-*used;*used+=add;}
}
static void hex_encode(const uint8_t *p,size_t n,char *out,size_t cap){static const char x[]="0123456789ABCDEF";size_t o=0U;for(size_t i=0;i<n&&o+2U<cap;++i){out[o++]=x[p[i]>>4];out[o++]=x[p[i]&15U];}out[o]='\0';}

size_t gw_config_export_csv(char *out,size_t cap)
{
    if((out==NULL)||(cap==0U))return 0U;size_t u=0U;out[0]='\0';gw_runtime_config_t r;gw_config_get_runtime(&r);
    append(out,cap,&u,"# GWCFG,1\nNET,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",r.dhcp?1U:0U,r.ip[0],r.ip[1],r.ip[2],r.ip[3],r.mask[0],r.mask[1],r.mask[2],r.mask[3],r.gateway[0],r.gateway[1],r.gateway[2],r.gateway[3]);
    append(out,cap,&u,"RS485,%lu,%u,%u,%u,%lu,%u\n",(unsigned long)r.rs485_baudrate,r.rs485_data_bits,r.rs485_parity,r.rs485_stop_bits,(unsigned long)r.rs485_timeout_ms,r.rs485_retry);
    append(out,cap,&u,"SERVICE,%s,%s,%u,%u,%s,0\n",r.sntp_server,r.syslog_server,r.syslog_port,r.syslog_min_level,r.snmp_community);
    gw_security_credential_t c;gw_security_get_credential(&c);char salt[33],hash[65];hex_encode(c.salt,sizeof(c.salt),salt,sizeof(salt));hex_encode(c.password_hash,sizeof(c.password_hash),hash,sizeof(hash));append(out,cap,&u,"AUTH,%s,%s,%s\n",c.username,salt,hash);
    gw_device_t d[GW_MAX_DEVICES];uint32_t dn=device_manager_snapshot(d,GW_MAX_DEVICES);for(uint32_t i=0;i<dn;++i)append(out,cap,&u,"DEVICE,%lu,%s,%u,%u,%u,%lu,%u,%u\n",(unsigned long)d[i].id,d[i].name,(unsigned)d[i].protocol,(unsigned)d[i].interface_id,(unsigned)d[i].address,(unsigned long)d[i].timeout_ms,(unsigned)d[i].retry,(d[i].state!=DEVICE_DISABLED)?1U:0U);
    gw_point_t p[32];for(uint32_t off=0;;off+=32U){uint32_t pn=point_db_snapshot_range(off,p,32U);if(pn==0U)break;for(uint32_t i=0;i<pn;++i)append(out,cap,&u,"POINT,%lu,%lu,%s,%u,%.9g,%.9g\n",(unsigned long)p[i].id,(unsigned long)p[i].device_id,p[i].name,(unsigned)p[i].type,(double)p[i].scale,(double)p[i].offset);if(pn<32U)break;}
    can_signal_map_t cm[64];uint32_t cn=can_decoder_snapshot(cm,64U);for(uint32_t i=0;i<cn;++i)append(out,cap,&u,"CANMAP,%lu,%lu,%lu,0x%lX,%u,%u,%u,%u,%u,%u\n",(unsigned long)cm[i].id,(unsigned long)cm[i].device_id,(unsigned long)cm[i].point_id,(unsigned long)cm[i].can_id,cm[i].byte_offset,(unsigned)cm[i].encoding,(unsigned)cm[i].endian,cm[i].extended?1U:0U,cm[i].require_fd?1U:0U,cm[i].enabled?1U:0U);
    poll_job_t pj[GW_MAX_POLL_JOBS];uint32_t pjn=poll_scheduler_snapshot(pj,GW_MAX_POLL_JOBS);for(uint32_t i=0;i<pjn;++i)append(out,cap,&u,"POLL,%lu,%lu,%lu,%u,%u,%u,%u,%lu,%u,%u\n",(unsigned long)pj[i].id,(unsigned long)pj[i].device_id,(unsigned long)pj[i].point_id,pj[i].function_code,pj[i].start_address,pj[i].quantity,pj[i].register_offset,(unsigned long)pj[i].interval_ms,(unsigned)pj[i].encoding,pj[i].enabled?1U:0U);
    gw_alarm_rule_t ar[GW_MAX_ALARM_RULES];uint32_t an=gw_alarm_snapshot(ar,GW_MAX_ALARM_RULES);for(uint32_t i=0;i<an;++i)append(out,cap,&u,"ALARM,%lu,%lu,%u,%.9g,%.9g,%u,%u\n",(unsigned long)ar[i].id,(unsigned long)ar[i].point_id,(unsigned)ar[i].kind,ar[i].threshold,ar[i].hysteresis,ar[i].priority,ar[i].enabled?1U:0U);
    gw_linkage_rule_t lr[GW_MAX_LINKAGE_RULES];uint32_t ln=gw_linkage_snapshot(lr,GW_MAX_LINKAGE_RULES);for(uint32_t i=0;i<ln;++i){char hex[129];hex_encode(lr[i].can_data,lr[i].can_len,hex,sizeof(hex));append(out,cap,&u,"RULE,%lu,%lu,%u,%.9g,%.9g,%lu,%u,%lu,0x%lX,%u,%u,%s,%u,%u,%u,%u\n",(unsigned long)lr[i].id,(unsigned long)lr[i].point_id,(unsigned)lr[i].op,lr[i].threshold,lr[i].hysteresis,(unsigned long)lr[i].require_device_id,(unsigned)lr[i].action,(unsigned long)lr[i].cooldown_ms,(unsigned long)lr[i].can_id,lr[i].can_extended?1U:0U,lr[i].can_fd?1U:0U,hex,lr[i].modbus_slave,lr[i].modbus_register,lr[i].modbus_value,lr[i].enabled?1U:0U);}
    if(u>=cap)out[cap-1U]='\0';s_stats.csv_length=(uint32_t)((u<cap)?u:cap-1U);return (u<cap)?u:cap-1U;
}

void gw_config_mark_dirty(void){s_stats.dirty=true;s_dirty_since=gw_time_ms();}
void gw_config_request_save(void){s_save_requested=true;gw_config_mark_dirty();}

gw_err_t gw_config_apply_csv_line(const char *line,bool persist)
{
    if(line==NULL)return GW_ERR_PARAM;char temp[CSV_LINE_MAX];if(strlen(line)>=sizeof(temp))return GW_ERR_FULL;safe_copy(temp,sizeof(temp),line);
    (void)xEventGroupSetBits(g_system_events,EVT_CONFIG_UPDATING);gw_err_t e=apply_line_mutable(temp);(void)xEventGroupClearBits(g_system_events,EVT_CONFIG_UPDATING);if((e==GW_OK)&&persist)gw_config_mark_dirty();return e;
}

static void delete_point_dependents(uint32_t point_id)
{
    can_signal_map_t cm[64];uint32_t cn=can_decoder_snapshot(cm,64U);for(uint32_t i=0U;i<cn;++i)if(cm[i].point_id==point_id)(void)can_decoder_remove(cm[i].id);
    poll_job_t pj[GW_MAX_POLL_JOBS];uint32_t pn=poll_scheduler_snapshot(pj,GW_MAX_POLL_JOBS);for(uint32_t i=0U;i<pn;++i)if(pj[i].point_id==point_id)(void)poll_scheduler_remove(pj[i].id);
    gw_alarm_rule_t ar[GW_MAX_ALARM_RULES];uint32_t an=gw_alarm_snapshot(ar,GW_MAX_ALARM_RULES);for(uint32_t i=0U;i<an;++i)if(ar[i].point_id==point_id)(void)gw_alarm_remove(ar[i].id);
    gw_linkage_rule_t lr[GW_MAX_LINKAGE_RULES];uint32_t ln=gw_linkage_snapshot(lr,GW_MAX_LINKAGE_RULES);for(uint32_t i=0U;i<ln;++i)if(lr[i].point_id==point_id)(void)gw_linkage_remove(lr[i].id);
}

static gw_err_t delete_point_cascade(uint32_t point_id)
{
    gw_point_t p;if(point_db_get(point_id,&p)!=GW_OK)return GW_ERR_NOT_FOUND;
    delete_point_dependents(point_id);
    return point_db_remove(point_id);
}

static gw_err_t delete_device_cascade(uint32_t device_id)
{
    gw_device_t d;if(device_manager_get(device_id,&d)!=GW_OK)return GW_ERR_NOT_FOUND;
    uint32_t offset=0U;
    for(;;){
        gw_point_t points[32];uint32_t n=point_db_snapshot_range(offset,points,32U);if(n==0U)break;
        uint32_t kept=0U;
        for(uint32_t i=0U;i<n;++i){
            if(points[i].device_id==device_id){(void)delete_point_cascade(points[i].id);}else{++kept;}
        }
        /* Deletions compact the logical snapshot, so advance only by entries
         * that survived this page. */
        offset+=kept;if(n<32U)break;
    }
    return device_manager_remove(device_id);
}

gw_err_t gw_config_delete(const char *kind,uint32_t id,bool persist)
{
    if((kind==NULL)||(id==0U))return GW_ERR_PARAM;gw_err_t e=GW_ERR_NOT_SUPPORTED;(void)xEventGroupSetBits(g_system_events,EVT_CONFIG_UPDATING);
    if(strcmp(kind,"DEVICE")==0)e=delete_device_cascade(id);else if(strcmp(kind,"POINT")==0)e=delete_point_cascade(id);else if(strcmp(kind,"CANMAP")==0)e=can_decoder_remove(id);else if(strcmp(kind,"POLL")==0)e=poll_scheduler_remove(id);else if(strcmp(kind,"ALARM")==0)e=gw_alarm_remove(id);else if(strcmp(kind,"RULE")==0)e=gw_linkage_remove(id);
    (void)xEventGroupClearBits(g_system_events,EVT_CONFIG_UPDATING);if((e==GW_OK)&&persist)gw_config_mark_dirty();return e;
}

gw_err_t gw_config_import_csv(const char *csv,size_t length,bool persist)
{
    if((csv==NULL)||(length==0U)||(length>GW_CONFIG_MAX_CSV_BYTES))return GW_ERR_PARAM;memcpy(s_csv,csv,length);s_csv[length]='\0';gw_err_t e=import_locked(s_csv,length);if((e==GW_OK)&&persist)gw_config_mark_dirty();return e;
}

static gw_err_t save_now(void)
{
    size_t n=gw_config_export_csv(s_csv,sizeof(s_csv));if(n==0U)return GW_ERR_IO;uint32_t gen=s_stats.generation+1U;
    gw_err_t ext=gw_config_external_save_csv(s_csv,n);gw_err_t e=gw_flash_config_save(s_csv,n,gen);
    if((e==GW_OK)||(ext==GW_OK)){s_stats.generation=gen;s_stats.dirty=false;s_save_requested=false;++s_stats.save_count;return GW_OK;}return e;
}

void gw_config_init(void)
{
    memset(&s_stats,0,sizeof(s_stats));defaults();gw_flash_store_init();gw_security_init();gw_automation_init();
    size_t n=0U;gw_err_t e=gw_config_external_load_csv(s_csv,sizeof(s_csv)-1U,&n);if(e==GW_OK&&n>0U){s_csv[n]='\0';if(import_locked(s_csv,n)==GW_OK){s_stats.loaded_from_external=true;s_stats.loaded_from_persistent=true;++s_stats.load_count;}}
    if(!s_stats.loaded_from_persistent){uint32_t gen=0U;e=gw_flash_config_load(s_csv,sizeof(s_csv)-1U,&n,&gen);if(e==GW_OK&&n>0U){s_csv[n]='\0';if(import_locked(s_csv,n)==GW_OK){s_stats.loaded_from_persistent=true;s_stats.generation=gen;++s_stats.load_count;}}}
    if(!s_stats.loaded_from_persistent){GW_LOGI("CFG","no persistent config; using compiled defaults");}
}

void gw_config_get_runtime(gw_runtime_config_t *out){runtime_load(out);}
void gw_config_get_stats(gw_config_stats_t *out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_stats;taskEXIT_CRITICAL();}
void gw_config_request_factory_reset(bool clear_spool){s_factory_reset_clear_spool=clear_spool;s_factory_reset_requested=true;if(g_system_events!=NULL)(void)xEventGroupSetBits(g_system_events,EVT_FACTORY_RESET_REQ);}

static void config_task(void *arg)
{
    (void)arg;uint64_t button_since=0U;for(;;){gw_watchdog_beat(GW_WD_CONFIG);gw_security_periodic();uint64_t now=gw_time_ms();
        if(gw_factory_button_pressed()){if(button_since==0U)button_since=now;else if(now-button_since>=3000U){s_factory_reset_clear_spool=true;s_factory_reset_requested=true;}}else button_since=0U;
        if(s_factory_reset_requested){++s_stats.factory_reset_count;GW_LOGW("CFG","factory reset requested");(void)gw_config_external_factory_reset();(void)gw_flash_config_factory_reset();if(s_factory_reset_clear_spool)(void)gw_flash_spool_clear();vTaskDelay(pdMS_TO_TICKS(GW_CONFIG_FACTORY_RESET_REBOOT_MS));NVIC_SystemReset();}
#if (GW_CONFIG_AUTOSAVE_ENABLE!=0U)
        if(s_stats.dirty&&(s_save_requested||((s_dirty_since!=0U)&&(now-s_dirty_since>=GW_CONFIG_AUTOSAVE_DELAY_MS)))){gw_err_t e=save_now();if(e!=GW_OK)GW_LOGE("CFG","persistent save failed err=%ld",(long)e);}
#endif
        vTaskDelay(pdMS_TO_TICKS(100U));}
}
void gw_config_task_create(void){TaskHandle_t h=xTaskCreateStatic(config_task,"config",CONFIG_TASK_STACK_WORDS,NULL,CONFIG_TASK_PRIORITY,s_stack,&s_tcb);configASSERT(h!=NULL);}

__attribute__((weak)) gw_err_t gw_config_external_load_csv(char *out,size_t cap,size_t *len){(void)out;(void)cap;if(len!=NULL)*len=0U;return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) gw_err_t gw_config_external_save_csv(const char *data,size_t len){(void)data;(void)len;return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) gw_err_t gw_config_external_factory_reset(void){return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) bool gw_factory_button_pressed(void){return false;}

#else
void gw_config_init(void){}
void gw_config_task_create(void){}
void gw_config_get_runtime(gw_runtime_config_t *o){(void)o;}
void gw_config_get_stats(gw_config_stats_t *o){(void)o;}
gw_err_t gw_config_import_csv(const char*c,size_t l,bool p){(void)c;(void)l;(void)p;return GW_ERR_NOT_SUPPORTED;}
gw_err_t gw_config_apply_csv_line(const char*l,bool p){(void)l;(void)p;return GW_ERR_NOT_SUPPORTED;}
gw_err_t gw_config_delete(const char*k,uint32_t i,bool p){(void)k;(void)i;(void)p;return GW_ERR_NOT_SUPPORTED;}
size_t gw_config_export_csv(char*o,size_t c){(void)o;(void)c;return 0U;}
void gw_config_mark_dirty(void){}void gw_config_request_save(void){}void gw_config_request_factory_reset(bool c){(void)c;}
__attribute__((weak)) gw_err_t gw_config_external_load_csv(char*o,size_t c,size_t*l){(void)o;(void)c;if(l)*l=0U;return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) gw_err_t gw_config_external_save_csv(const char*d,size_t l){(void)d;(void)l;return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) gw_err_t gw_config_external_factory_reset(void){return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) bool gw_factory_button_pressed(void){return false;}
#endif
