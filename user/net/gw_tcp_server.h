#ifndef GW_TCP_SERVER_H
#define GW_TCP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool task_started;
    bool listening;
    bool client_connected;
    uint16_t listen_port;
    uint32_t listener_start_count;
    uint32_t listener_restart_count;
    uint32_t accept_count;
    uint32_t disconnect_count;
    uint32_t accept_error_count;
    uint32_t recv_count;
    uint32_t recv_error_count;
    uint32_t rx_bytes;
    uint32_t send_count;
    uint32_t send_error_count;
    uint32_t tx_bytes;
} gw_tcp_server_stats_t;

void gw_tcp_server_task_create(void);
void gw_tcp_server_get_stats(gw_tcp_server_stats_t *out);

#endif /* GW_TCP_SERVER_H */
