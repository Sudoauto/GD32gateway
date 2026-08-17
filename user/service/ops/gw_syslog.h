#ifndef GW_SYSLOG_H
#define GW_SYSLOG_H
#include <stddef.h>
#include <stdint.h>
void gw_syslog_init(void);
void gw_syslog_task_create(void);
void gw_syslog_capture(const char *level,const char *tag,const char *line,size_t length);
typedef struct{uint32_t queued;uint32_t dropped;uint32_t sent;uint32_t send_errors;}gw_syslog_stats_t;
void gw_syslog_get_stats(gw_syslog_stats_t *out);
#endif
