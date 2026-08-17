#ifndef GW_SNMP_H
#define GW_SNMP_H
#include <stdint.h>
typedef struct{uint32_t requests;uint32_t responses;uint32_t auth_reject;uint32_t parse_error;}gw_snmp_stats_t;
void gw_snmp_init(void);void gw_snmp_task_create(void);void gw_snmp_get_stats(gw_snmp_stats_t*out);
#endif
