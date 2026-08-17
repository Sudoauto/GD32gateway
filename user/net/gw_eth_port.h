#ifndef GW_ETH_PORT_H
#define GW_ETH_PORT_H

#include <stdbool.h>
#include <stdint.h>

bool gw_eth_port_prepare(void);
bool gw_eth_port_link_up(void);
bool gw_eth_port_mac_init(void);
void gw_eth_port_update_mac_from_phy(void);
bool gw_eth_port_get_link_mode(bool *speed_100m, bool *full_duplex);
void gw_eth_port_irq_enable(void);
void gw_eth_port_irq_disable(void);
uint16_t gw_eth_port_phy_id1(void);
uint16_t gw_eth_port_phy_id2(void);
bool gw_eth_port_phy_identity_ok(void);

#endif /* GW_ETH_PORT_H */
