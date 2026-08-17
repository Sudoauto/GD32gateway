#ifndef GW_NET_MANAGER_H
#define GW_NET_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool stack_started;
    bool link_up;
    bool ip_ready;
    uint32_t link_up_count;
    uint32_t link_down_count;
    uint32_t init_fail_count;
    uint32_t ipv4_addr;
} gw_net_status_t;

void gw_net_task_create(void);
void gw_net_get_status(gw_net_status_t *out);

#endif /* GW_NET_MANAGER_H */
