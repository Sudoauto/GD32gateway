#ifndef GW_SNTP_H
#define GW_SNTP_H

#include <stdbool.h>
#include <stdint.h>

void gw_sntp_init(void);
void gw_sntp_task_create(void);
void gw_sntp_set_server(const char *server);
bool gw_sntp_running(void);
uint32_t gw_sntp_restart_count(void);

#endif
