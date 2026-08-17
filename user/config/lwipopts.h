#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#include "gateway_build_config.h"

/* GD32H759 gateway lwIP 2.1.2 configuration.
 * Baseline goal: reliable Ethernet/IPv4 transport for later MQTT/HTTP/Modbus-TCP.
 * Keep the first Ethernet milestone conservative: one ENET0/RMII interface,
 * ARP/ICMP/TCP/UDP, optional DHCP, netconn API, no sockets and no IPv6 yet. */

#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (15U * 1024U)
/* Dedicated 16 KiB SRAM region reserved by the Keil target. */
#define LWIP_RAM_HEAP_POINTER           ((void *)0x30004000UL)

#define MEMP_NUM_PBUF                   24
#define MEMP_NUM_UDP_PCB                6
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                24
#define MEMP_NUM_SYS_TIMEOUT            12
#define MEMP_NUM_NETBUF                 8
#define MEMP_NUM_NETCONN                8

#define PBUF_POOL_SIZE                  12
#define PBUF_POOL_BUFSIZE               1536

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ICMP                       1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DNS                        1
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0

#define TCP_TTL                         64
#define TCP_QUEUE_OOSEQ                 0
#define TCP_MSS                         1460
#define TCP_SND_BUF                     (4U * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4U * TCP_SND_BUF) / TCP_MSS)
#define TCP_WND                         (4U * TCP_MSS)
#define UDP_TTL                         64

#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1

#define LWIP_NETCONN                    1
#define LWIP_NETIF_API                  1
#define LWIP_SOCKET                     0
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_PROVIDE_ERRNO              1
#define LWIP_STATS                      0
#define LWIP_DEBUG                      0

/* ENET hardware checksum offload is enabled in the gateway port. */
#define CHECKSUM_BY_HARDWARE            1
#define CHECKSUM_GEN_IP                 0
#define CHECKSUM_GEN_UDP                0
#define CHECKSUM_GEN_TCP                0
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_CHECK_TCP              0
#define CHECKSUM_GEN_ICMP               0

#include "FreeRTOSConfig.h"

#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          2048
#define TCPIP_MBOX_SIZE                 16
#define TCPIP_THREAD_PRIO               3
#define DEFAULT_THREAD_STACKSIZE        1024
#define DEFAULT_TCP_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_ACCEPTMBOX_SIZE         8
#define LWIP_COMPAT_MUTEX               1

/* Enable the FreeRTOS port's wrong-thread assertions for raw lwIP APIs. */
void sys_mark_tcpip_thread(void);
void sys_check_core_locking(void);
#define LWIP_MARK_TCPIP_THREAD()        sys_mark_tcpip_thread()
#define LWIP_ASSERT_CORE_LOCKED()       sys_check_core_locking()

/* v0.9.0 SNTP client. The callback updates the gateway UTC service while
 * uptime remains monotonic and independent. */
#define SNTP_MAX_SERVERS                1
#define SNTP_SERVER_DNS                 1
#define SNTP_CHECK_RESPONSE             2
#define SNTP_UPDATE_DELAY               GW_SNTP_SYNC_PERIOD_MS
extern void gw_time_set_utc_seconds(unsigned int unix_seconds);
#define SNTP_SET_SYSTEM_TIME(sec)       gw_time_set_utc_seconds((unsigned int)(sec))

#endif /* LWIPOPTS_H */
