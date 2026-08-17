#!/usr/bin/env python3
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]

def text(rel):
    return (ROOT / rel).read_text(encoding='utf-8', errors='replace')

def require(cond, msg):
    if not cond:
        raise AssertionError(msg)

cfg = text('user/config/gateway_build_config.h')
lw = text('user/config/lwipopts.h')
port = text('user/net/gw_eth_port.c')
ethif = text('user/net/gw_ethernetif.c')
net = text('user/net/gw_net_manager.c')
lwport = text('user/net/gw_lwip_port.c')
archcc = text('user/config/arch/cc.h')
cache = text('user/bsp/bsp_cache.c')
app = text('user/app/gateway_app.c')
irq = text('user/interrupt/gateway_irq.c')
eneth = text('Firmware/GD32H7xx_standard_peripheral/Include/gd32h7xx_enet.h')

require(re.search(r'#define\s+GW_ETH_ENABLE\s+1U', cfg), 'Ethernet must be enabled in v0.6 baseline')
require(re.search(r'#define\s+GW_ETH_PHY_ADDRESS\s+0U', cfg), 'LAN8720A PHY address must be 0 for supplied board example')
require(re.search(r'#define\s+GW_ETH_DHCP_ENABLE\s+0U', cfg), 'baseline must use deterministic static IP')
require(all(re.search(rf'#define\s+{name}\s+{value}U', cfg) for name, value in [('GW_ETH_IP0',192),('GW_ETH_IP1',168),('GW_ETH_IP2',103),('GW_ETH_IP3',213)]), 'static IP must be 192.168.103.213')
require(re.search(r'#define\s+GW_CANFD_BRS_ENABLE\s+0U', cfg), 'Ethernet work must not regress BRS-OFF CAN baseline')

require(re.search(r'#define\s+PHY_TYPE\s+LAN8700\b', eneth), 'SPL PHY type must match LAN8720A/LAN8700 status layout')
require(re.search(r'#define\s+PHY_ADDRESS\s+\(\(uint16_t\)0U\)', eneth), 'SPL PHY address must be 0')

for pin in ['GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7', 'GPIO_PIN_11', 'GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5', 'GPIO_PIN_13 | GPIO_PIN_14', 'GPIO_PIN_6']:
    require(pin in port, f'missing expected RMII/reset pin configuration: {pin}')
require('RCU_CKOUT0SRC_PLL0P, RCU_CKOUT0_DIV12' in port, 'PHY 50 MHz RMII clock must be PLL0P/12')
require('SYSCFG_ENET_PHY_RMII' in port, 'ENET0 must use RMII')
require('enet_phy_config(ENET0)' in port, 'PHY/SMI init must be explicit and cable-safe')
require('while (!gw_eth_port_link_up())' in net, 'network task must tolerate unplugged Ethernet at boot')

require('0x30000000UL' in cache and 'MPU_REGION_SIZE_16KB' in cache, 'ENET DMA 16 KiB MPU window missing')
require('MPU_ACCESS_NON_CACHEABLE' in cache, 'ENET DMA window must be non-cacheable')
require('0x30004000UL' in lw and 'LWIP_RAM_HEAP_POINTER' in lw, 'lwIP heap must use reserved 0x30004000 SRAM')
require(re.search(r'#define\s+MEM_SIZE\s+\(15U \* 1024U\)', lw), 'lwIP heap size must remain within 16 KiB reserved window')
require(re.search(r'#define\s+LWIP_IPV4\s+1', lw) and re.search(r'#define\s+LWIP_TCP\s+1', lw), 'IPv4/TCP stack must be enabled')
require(re.search(r'#define\s+LWIP_NETCONN\s+1', lw), 'netconn API must be enabled for northbound clients')
require('LWIP_RAND()' in archcc and 'gw_lwip_rand' in lwport, 'lwIP DNS/TCP random source must be provided')
require('LWIP_PLATFORM_ASSERT' in archcc and 'gw_lwip_platform_assert' in lwport, 'lwIP assertions must not be compiled to no-op')

require('ETH_TX_WAIT_MS' in ethif and 'tx_busy_timeout' in ethif, 'TX path needs bounded descriptor wait')
require('ETH_RX_BURST_MAX' in ethif, 'RX task needs bounded burst to avoid southbound starvation')
require('gw_eth_port_irq_enable();' in ethif, 'ENET RX IRQ must be enabled after descriptors/task exist')
require('ENET0_IRQHandler' in irq and 'gw_ethernetif_isr();' in irq, 'ENET0 IRQ must be routed to gateway Ethernet ISR')
require('gw_net_task_create();' in app, 'network manager task must be created by gateway app')
require('tcpip_init(NULL, NULL);' in net and 'netifapi_netif_add' in net, 'lwIP tcpip thread/netif startup missing')
require('EVT_NET_IP_READY' in text('user/rtos/rtos_objects.h'), 'IP-ready event is required for later northbound tasks')

# Do not silently import the official TCP demo into production startup.
all_user = '\n'.join(p.read_text(encoding='utf-8', errors='replace') for p in (ROOT/'user').rglob('*.c'))
require('tcp_client_init(' not in all_user and 'tcp_client_task' not in all_user, 'official TCP demo must not auto-run in baseline')

proj = ET.parse(ROOT/'project/gateway.uvprojx')
paths = [e.text or '' for e in proj.iter('FilePath')]
joined = '\n'.join(paths).replace('\\','/')
for required in [
    '../user/net/gw_eth_port.c', '../user/net/gw_ethernetif.c', '../user/net/gw_net_manager.c', '../user/net/gw_lwip_port.c',
    '../Firmware/GD32H7xx_standard_peripheral/Source/gd32h7xx_enet.c',
    '../vendor/lwip-2.1.2/src/api/tcpip.c', '../vendor/lwip-2.1.2/src/core/init.c',
    '../vendor/lwip-2.1.2/src/netif/ethernet.c', '../vendor/lwip-2.1.2/port/GD32H7xx/FreeRTOS/sys_arch.c']:
    require(required in joined, f'Keil project missing {required}')

print('Ethernet/lwIP baseline regression: PASS')
