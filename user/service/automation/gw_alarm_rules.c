#include "gw_alarm_rules.h"

#include <math.h>
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "device_manager.h"
#include "gateway_build_config.h"
#include "gw_command_router.h"
#include "gw_log.h"
#include "gw_time.h"
#include "gw_uplink.h"
#include "point_db.h"
#include "rtos_objects.h"

typedef struct {
    gw_alarm_rule_t cfg;
    bool used;
    bool active;
    bool have_previous;
    double previous_value;
    uint64_t previous_ms;
} alarm_slot_t;

typedef struct {
    gw_linkage_rule_t cfg;
    bool used;
    bool latched;
    uint64_t last_trigger_ms;
} linkage_slot_t;

static alarm_slot_t s_alarm[GW_MAX_ALARM_RULES];
static linkage_slot_t s_linkage[GW_MAX_LINKAGE_RULES];
static gw_automation_stats_t s_stats;

static bool point_as_double(const gw_point_t *point, double *out)
{
    if ((point == NULL) || (out == NULL)) return false;
    switch (point->type) {
    case GW_VALUE_BOOL: *out = point->value.b ? 1.0 : 0.0; return true;
    case GW_VALUE_U16: *out = (double)point->value.u16; return true;
    case GW_VALUE_I16: *out = (double)point->value.i16; return true;
    case GW_VALUE_U32: *out = (double)point->value.u32; return true;
    case GW_VALUE_I32: *out = (double)point->value.i32; return true;
    case GW_VALUE_F32: *out = (double)point->value.f32; return isfinite(*out);
    case GW_VALUE_F64: *out = point->value.f64; return isfinite(*out);
    default: return false;
    }
}

static bool rule_condition(gw_rule_operator_t op, double value, double threshold)
{
    switch (op) {
    case GW_RULE_OP_GT: return value > threshold;
    case GW_RULE_OP_GE: return value >= threshold;
    case GW_RULE_OP_LT: return value < threshold;
    case GW_RULE_OP_LE: return value <= threshold;
    default: return false;
    }
}

static bool rule_release(gw_rule_operator_t op, double value, double threshold, double hysteresis)
{
    if (hysteresis < 0.0) hysteresis = -hysteresis;
    switch (op) {
    case GW_RULE_OP_GT:
    case GW_RULE_OP_GE: return value <= (threshold - hysteresis);
    case GW_RULE_OP_LT:
    case GW_RULE_OP_LE: return value >= (threshold + hysteresis);
    default: return true;
    }
}

void gw_automation_init(void)
{
    memset(s_alarm, 0, sizeof(s_alarm));
    memset(s_linkage, 0, sizeof(s_linkage));
    memset(&s_stats, 0, sizeof(s_stats));
}

static gw_err_t validate_alarm(const gw_alarm_rule_t *r)
{
    if ((r == NULL) || (r->id == 0U) || (r->point_id == 0U) ||
        !isfinite(r->threshold) || !isfinite(r->hysteresis) ||
        (r->hysteresis < 0.0) || (r->priority > 7U) ||
        (r->kind > GW_ALARM_RATE_HIGH)) return GW_ERR_PARAM;
    gw_point_t p;
    return (point_db_get(r->point_id, &p) == GW_OK) ? GW_OK : GW_ERR_NOT_FOUND;
}

gw_err_t gw_alarm_upsert(const gw_alarm_rule_t *rule)
{
#if (GW_ALARM_ENABLE == 0U)
    (void)rule; return GW_ERR_NOT_SUPPORTED;
#else
    gw_err_t e = validate_alarm(rule); if (e != GW_OK) return e;
    if ((config_db_mutex == NULL) ||
        (xSemaphoreTake(config_db_mutex, pdMS_TO_TICKS(20U)) != pdTRUE)) return GW_ERR_BUSY;
    int32_t free_idx = -1;
    for (uint32_t i=0U;i<GW_MAX_ALARM_RULES;++i) {
        if (s_alarm[i].used && s_alarm[i].cfg.id == rule->id) {
            bool keep_active=s_alarm[i].active, keep_prev=s_alarm[i].have_previous;
            double pv=s_alarm[i].previous_value; uint64_t pt=s_alarm[i].previous_ms;
            memset(&s_alarm[i],0,sizeof(s_alarm[i])); s_alarm[i].cfg=*rule; s_alarm[i].used=true;
            s_alarm[i].active=keep_active;s_alarm[i].have_previous=keep_prev;s_alarm[i].previous_value=pv;s_alarm[i].previous_ms=pt;
            (void)xSemaphoreGive(config_db_mutex); return GW_OK;
        }
        if (!s_alarm[i].used && free_idx < 0) free_idx=(int32_t)i;
    }
    if (free_idx < 0) { (void)xSemaphoreGive(config_db_mutex); return GW_ERR_FULL; }
    s_alarm[free_idx].cfg=*rule; s_alarm[free_idx].used=true;
    (void)xSemaphoreGive(config_db_mutex); return GW_OK;
#endif
}

gw_err_t gw_alarm_remove(uint32_t id)
{
    if ((id==0U)||(config_db_mutex==NULL)) return GW_ERR_PARAM;
    if (xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(20U))!=pdTRUE) return GW_ERR_BUSY;
    for(uint32_t i=0U;i<GW_MAX_ALARM_RULES;++i) if(s_alarm[i].used&&s_alarm[i].cfg.id==id){memset(&s_alarm[i],0,sizeof(s_alarm[i]));(void)xSemaphoreGive(config_db_mutex);return GW_OK;}
    (void)xSemaphoreGive(config_db_mutex);return GW_ERR_NOT_FOUND;
}

uint32_t gw_alarm_snapshot(gw_alarm_rule_t *out,uint32_t max_count)
{
    if((out==NULL)||(max_count==0U)||(config_db_mutex==NULL))return 0U;
    if(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(20U))!=pdTRUE)return 0U;
    uint32_t n=0U;for(uint32_t i=0U;i<GW_MAX_ALARM_RULES&&n<max_count;++i)if(s_alarm[i].used)out[n++]=s_alarm[i].cfg;
    (void)xSemaphoreGive(config_db_mutex);return n;
}

static gw_err_t validate_linkage(const gw_linkage_rule_t *r)
{
    if((r==NULL)||(r->id==0U)||(r->point_id==0U)||!isfinite(r->threshold)||!isfinite(r->hysteresis)||
       (r->hysteresis<0.0)||(r->op>GW_RULE_OP_LE)||(r->action>GW_RULE_ACTION_MODBUS_FC06))return GW_ERR_PARAM;
    gw_point_t p;if(point_db_get(r->point_id,&p)!=GW_OK)return GW_ERR_NOT_FOUND;
    if(r->action==GW_RULE_ACTION_CAN){if((r->can_len>64U)||(r->can_id>(r->can_extended?0x1FFFFFFFU:0x7FFU)))return GW_ERR_PARAM;}
    else if((r->modbus_slave==0U)||(r->modbus_slave>247U))return GW_ERR_PARAM;
    return GW_OK;
}

gw_err_t gw_linkage_upsert(const gw_linkage_rule_t *rule)
{
#if (GW_RULE_ENGINE_ENABLE == 0U)
    (void)rule;return GW_ERR_NOT_SUPPORTED;
#else
    gw_err_t e=validate_linkage(rule);if(e!=GW_OK)return e;
    if((config_db_mutex==NULL)||(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(20U))!=pdTRUE))return GW_ERR_BUSY;
    int32_t free_idx=-1;for(uint32_t i=0U;i<GW_MAX_LINKAGE_RULES;++i){
        if(s_linkage[i].used&&s_linkage[i].cfg.id==rule->id){bool lat=s_linkage[i].latched;uint64_t last=s_linkage[i].last_trigger_ms;memset(&s_linkage[i],0,sizeof(s_linkage[i]));s_linkage[i].cfg=*rule;s_linkage[i].used=true;s_linkage[i].latched=lat;s_linkage[i].last_trigger_ms=last;(void)xSemaphoreGive(config_db_mutex);return GW_OK;}
        if(!s_linkage[i].used&&free_idx<0)free_idx=(int32_t)i;
    }
    if(free_idx<0){(void)xSemaphoreGive(config_db_mutex);return GW_ERR_FULL;}s_linkage[free_idx].cfg=*rule;s_linkage[free_idx].used=true;(void)xSemaphoreGive(config_db_mutex);return GW_OK;
#endif
}

gw_err_t gw_linkage_remove(uint32_t id)
{
    if((id==0U)||(config_db_mutex==NULL))return GW_ERR_PARAM;if(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(20U))!=pdTRUE)return GW_ERR_BUSY;
    for(uint32_t i=0U;i<GW_MAX_LINKAGE_RULES;++i)if(s_linkage[i].used&&s_linkage[i].cfg.id==id){memset(&s_linkage[i],0,sizeof(s_linkage[i]));(void)xSemaphoreGive(config_db_mutex);return GW_OK;}
    (void)xSemaphoreGive(config_db_mutex);return GW_ERR_NOT_FOUND;
}

uint32_t gw_linkage_snapshot(gw_linkage_rule_t *out,uint32_t max_count)
{
    if((out==NULL)||(max_count==0U)||(config_db_mutex==NULL))return 0U;if(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(20U))!=pdTRUE)return 0U;
    uint32_t n=0U;for(uint32_t i=0U;i<GW_MAX_LINKAGE_RULES&&n<max_count;++i)if(s_linkage[i].used)out[n++]=s_linkage[i].cfg;(void)xSemaphoreGive(config_db_mutex);return n;
}

void gw_automation_reset(void)
{
    if((config_db_mutex!=NULL)&&(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(50U))==pdTRUE)){memset(s_alarm,0,sizeof(s_alarm));memset(s_linkage,0,sizeof(s_linkage));(void)xSemaphoreGive(config_db_mutex);}
}

static void eval_alarm_slot(alarm_slot_t *a,const gw_point_t *p,double value,uint64_t now)
{
    if(!a->used||!a->cfg.enabled||a->cfg.point_id!=p->id)return;
    bool next=a->active;double metric=value;
    if(a->cfg.kind==GW_ALARM_RATE_HIGH){
        if(!a->have_previous||now<=a->previous_ms){a->have_previous=true;a->previous_value=value;a->previous_ms=now;return;}
        double dt=(double)(now-a->previous_ms)/1000.0;metric=fabs((value-a->previous_value)/dt);a->previous_value=value;a->previous_ms=now;
        next=a->active ? (metric > (a->cfg.threshold-a->cfg.hysteresis)) : (metric >= a->cfg.threshold);
    }else if(a->cfg.kind==GW_ALARM_HIGH){next=a->active?(value>(a->cfg.threshold-a->cfg.hysteresis)):(value>=a->cfg.threshold);}
    else {next=a->active?(value<(a->cfg.threshold+a->cfg.hysteresis)):(value<=a->cfg.threshold);}
    if(next!=a->active){a->active=next;if(next)++s_stats.alarm_raised;else ++s_stats.alarm_cleared;gw_uplink_publish_alarm(a->cfg.id,p->id,a->cfg.kind,next,a->cfg.priority,metric,a->cfg.threshold);}
}

static void eval_linkage_slot(linkage_slot_t *r,const gw_point_t *p,double value,uint64_t now)
{
    if(!r->used||!r->cfg.enabled||r->cfg.point_id!=p->id)return;
    bool cond=rule_condition(r->cfg.op,value,r->cfg.threshold);
    if(r->latched){if(rule_release(r->cfg.op,value,r->cfg.threshold,r->cfg.hysteresis))r->latched=false;return;}
    if(!cond)return;
    if(r->cfg.require_device_id!=0U){gw_device_t d;if((device_manager_get(r->cfg.require_device_id,&d)!=GW_OK)||(d.state!=DEVICE_ONLINE)){++s_stats.rule_suppressed;return;}}
    uint32_t cooldown=(r->cfg.cooldown_ms!=0U)?r->cfg.cooldown_ms:GW_RULE_MIN_RETRIGGER_MS;
    if((r->last_trigger_ms!=0U)&&(now-r->last_trigger_ms<cooldown)){++s_stats.rule_suppressed;return;}
    gw_err_t e;
    if(r->cfg.action==GW_RULE_ACTION_CAN)e=gw_command_send_can(r->cfg.can_id,r->cfg.can_extended,r->cfg.can_fd,r->cfg.can_data,r->cfg.can_len);
    else e=gw_command_modbus_write_single_slave(r->cfg.modbus_slave,r->cfg.modbus_register,r->cfg.modbus_value);
    r->last_trigger_ms=now;r->latched=true;if(e==GW_OK){++s_stats.rule_triggered;gw_uplink_publish_rule_action(r->cfg.id,p->id,e);}else{++s_stats.rule_action_error;gw_uplink_publish_rule_action(r->cfg.id,p->id,e);}
}

void gw_automation_on_point(uint32_t point_id)
{
#if ((GW_ALARM_ENABLE == 0U) && (GW_RULE_ENGINE_ENABLE == 0U))
    (void)point_id;
#else
    gw_point_t p;if(point_db_get(point_id,&p)!=GW_OK||p.quality!=GW_QUALITY_GOOD)return;double value;if(!point_as_double(&p,&value)){++s_stats.alarm_eval_error;return;}uint64_t now=gw_time_ms();
    if((config_db_mutex==NULL)||(xSemaphoreTake(config_db_mutex,pdMS_TO_TICKS(10U))!=pdTRUE)){++s_stats.alarm_eval_error;return;}
    for(uint32_t i=0U;i<GW_MAX_ALARM_RULES;++i)eval_alarm_slot(&s_alarm[i],&p,value,now);
    for(uint32_t i=0U;i<GW_MAX_LINKAGE_RULES;++i)eval_linkage_slot(&s_linkage[i],&p,value,now);
    (void)xSemaphoreGive(config_db_mutex);
#endif
}

void gw_automation_get_stats(gw_automation_stats_t *out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_stats;taskEXIT_CRITICAL();}
