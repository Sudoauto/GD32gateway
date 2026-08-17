#ifndef GW_ETHERNETIF_H
#define GW_ETHERNETIF_H

#include <stdint.h>
#include "lwip/err.h"
#include "lwip/netif.h"

typedef struct {
    uint32_t rx_irq;
    uint32_t rx_frames;
    uint32_t rx_alloc_fail;
    uint32_t rx_input_fail;
    uint32_t tx_frames;
    uint32_t tx_fail;
    uint32_t tx_busy_timeout;
} gw_ethernetif_stats_t;

err_t gw_ethernetif_init(struct netif *netif);
void gw_ethernetif_isr(void);
void gw_ethernetif_get_stats(gw_ethernetif_stats_t *out);

#endif /* GW_ETHERNETIF_H */
