#include "gw_net_manager.h"
#include <string.h>
#include "gateway_build_config.h"
#include "gw_eth_port.h"
#include "gw_config.h"
#include "gw_ethernetif.h"
#include "gw_log.h"
#include "gw_watchdog.h"
#include "rtos_objects.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "netif/ethernet.h"

#if (GW_ETH_ENABLE != 0U)

#define NET_TASK_STACK_WORDS  1024U
#define NET_TASK_PRIORITY     2U
#define NET_LINK_POLL_MS      500U
#define NET_RETRY_MS          1000U

static StaticTask_t s_net_tcb;
static StackType_t s_net_stack[NET_TASK_STACK_WORDS];
static struct netif s_netif;
static gw_net_status_t s_status;

static void set_ip_event(bool ready)
{
    s_status.ip_ready = ready;
    if (ready) {
        (void)xEventGroupSetBits(g_system_events, EVT_NET_IP_READY);
    } else {
        (void)xEventGroupClearBits(g_system_events, EVT_NET_IP_READY);
    }
}

static void log_ipv4(const char *prefix)
{
    const ip4_addr_t *ip = netif_ip4_addr(&s_netif);
    const ip4_addr_t *nm = netif_ip4_netmask(&s_netif);
    const ip4_addr_t *gw = netif_ip4_gw(&s_netif);
    GW_LOGI("NET", "%s IP=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u",
            prefix,
            (unsigned)ip4_addr1(ip), (unsigned)ip4_addr2(ip),
            (unsigned)ip4_addr3(ip), (unsigned)ip4_addr4(ip),
            (unsigned)ip4_addr1(nm), (unsigned)ip4_addr2(nm),
            (unsigned)ip4_addr3(nm), (unsigned)ip4_addr4(nm),
            (unsigned)ip4_addr1(gw), (unsigned)ip4_addr2(gw),
            (unsigned)ip4_addr3(gw), (unsigned)ip4_addr4(gw));
}

static void log_link_mode(void)
{
    bool speed_100m = false;
    bool full_duplex = false;
    if (gw_eth_port_get_link_mode(&speed_100m, &full_duplex)) {
        GW_LOGI("ETH", "link negotiated: %s %s-duplex",
                speed_100m ? "100M" : "10M",
                full_duplex ? "full" : "half");
    }
}

static void runtime_ip_values(const gw_runtime_config_t *cfg, ip4_addr_t *ip,
                              ip4_addr_t *mask, ip4_addr_t *gw)
{
    if (cfg->dhcp) {
        ip4_addr_set_zero(ip); ip4_addr_set_zero(mask); ip4_addr_set_zero(gw);
    } else {
        IP4_ADDR(ip,cfg->ip[0],cfg->ip[1],cfg->ip[2],cfg->ip[3]);
        IP4_ADDR(mask,cfg->mask[0],cfg->mask[1],cfg->mask[2],cfg->mask[3]);
        IP4_ADDR(gw,cfg->gateway[0],cfg->gateway[1],cfg->gateway[2],cfg->gateway[3]);
    }
}

static bool start_lwip(const gw_runtime_config_t *cfg)
{
    ip4_addr_t ip, mask, gw;
    runtime_ip_values(cfg,&ip,&mask,&gw);

    tcpip_init(NULL, NULL);
    if (netifapi_netif_add(&s_netif, &ip, &mask, &gw, NULL,
                           gw_ethernetif_init, tcpip_input) != ERR_OK) {
        GW_LOGE("NET", "netif add failed"); return false;
    }
    (void)netifapi_netif_set_default(&s_netif);
    (void)netifapi_netif_set_link_up(&s_netif);
    (void)netifapi_netif_set_up(&s_netif);
    s_status.stack_started=true;s_status.link_up=true;++s_status.link_up_count;
    (void)xEventGroupSetBits(g_system_events,EVT_ETH_LINK_UP);

    if (cfg->dhcp) {
        if (netifapi_dhcp_start(&s_netif) != ERR_OK) { GW_LOGE("NET","DHCP start failed"); return false; }
        set_ip_event(false);GW_LOGI("NET","link UP, DHCP requesting address");
    } else {
        set_ip_event(true);s_status.ipv4_addr=ip4_addr_get_u32(netif_ip4_addr(&s_netif));log_ipv4("link UP");
    }
    return true;
}

static void apply_runtime_network(const gw_runtime_config_t *old_cfg,
                                  const gw_runtime_config_t *new_cfg)
{
    bool changed = old_cfg->dhcp != new_cfg->dhcp ||
                   memcmp(old_cfg->ip,new_cfg->ip,4U)!=0 ||
                   memcmp(old_cfg->mask,new_cfg->mask,4U)!=0 ||
                   memcmp(old_cfg->gateway,new_cfg->gateway,4U)!=0;
    if(!changed)return;
    if(new_cfg->dhcp){
        (void)netifapi_dhcp_stop(&s_netif);
        ip4_addr_t zero;ip4_addr_set_zero(&zero);
        (void)netifapi_netif_set_addr(&s_netif,&zero,&zero,&zero);
        (void)netifapi_dhcp_start(&s_netif);set_ip_event(false);
        GW_LOGI("NET","runtime config switched to DHCP");
    }else{
        (void)netifapi_dhcp_stop(&s_netif);
        ip4_addr_t ip,mask,gw;runtime_ip_values(new_cfg,&ip,&mask,&gw);
        (void)netifapi_netif_set_addr(&s_netif,&ip,&mask,&gw);
        s_status.ipv4_addr=ip4_addr_get_u32(&ip);set_ip_event(true);log_ipv4("runtime IP applied");
    }
}

static void network_task(void *argument)
{
    (void)argument;
    (void)xEventGroupWaitBits(g_system_events, EVT_CONFIG_READY,
                              pdFALSE, pdTRUE, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));

    for (;;) {
        gw_watchdog_beat(GW_WD_NET);
        if (!gw_eth_port_prepare()) {
            ++s_status.init_fail_count;
            GW_LOGE("NET", "Ethernet prepare failed; retrying");
            vTaskDelay(pdMS_TO_TICKS(NET_RETRY_MS));
            continue;
        }
        uint16_t phy_id1 = gw_eth_port_phy_id1();
        uint16_t phy_id2 = gw_eth_port_phy_id2();
        GW_LOGI("ETH", "LAN8720A/ENET0 RMII PHY id=%04X:%04X",
                (unsigned)phy_id1, (unsigned)phy_id2);
        if (!gw_eth_port_phy_identity_ok()) {
            ++s_status.init_fail_count;
            GW_LOGE("ETH", "unexpected PHY identity; expected LAN8720A at addr %u",
                    (unsigned)GW_ETH_PHY_ADDRESS);
            vTaskDelay(pdMS_TO_TICKS(NET_RETRY_MS));
            continue;
        }
        break;
    }

    bool waiting_logged = false;
    while (!gw_eth_port_link_up()) {
        gw_watchdog_beat(GW_WD_NET);
        if (!waiting_logged) {
            GW_LOGI("NET", "Ethernet cable/link DOWN; waiting for link");
            waiting_logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(NET_LINK_POLL_MS));
    }

    while (!gw_eth_port_mac_init()) {
        gw_watchdog_beat(GW_WD_NET);
        ++s_status.init_fail_count;
        GW_LOGE("NET", "PHY auto-negotiation/MAC init failed; retrying");
        vTaskDelay(pdMS_TO_TICKS(NET_RETRY_MS));
    }
    gw_eth_port_update_mac_from_phy();
    log_link_mode();

    gw_runtime_config_t active_cfg;
    gw_config_get_runtime(&active_cfg);
    if (!start_lwip(&active_cfg)) {
        ++s_status.init_fail_count;
        GW_LOGE("NET", "lwIP start failed; network task stopped");
        vTaskDelete(NULL);
        return;
    }

    bool last_link = true;
    TickType_t next_diag = xTaskGetTickCount() + pdMS_TO_TICKS(GW_ETH_DIAG_PERIOD_MS);
    for (;;) {
        gw_watchdog_beat(GW_WD_NET);
        bool link = gw_eth_port_link_up();
        if (link != last_link) {
            last_link = link;
            s_status.link_up = link;
            if (link) {
                ++s_status.link_up_count;
                gw_eth_port_update_mac_from_phy();
                log_link_mode();
                (void)netifapi_netif_set_link_up(&s_netif);
                (void)xEventGroupSetBits(g_system_events, EVT_ETH_LINK_UP);
                if(active_cfg.dhcp){(void)netifapi_dhcp_start(&s_netif);set_ip_event(false);}else set_ip_event(true);
                GW_LOGI("NET", "Ethernet link UP");
            } else {
                ++s_status.link_down_count;
                (void)netifapi_netif_set_link_down(&s_netif);
                (void)xEventGroupClearBits(g_system_events, EVT_ETH_LINK_UP);
                set_ip_event(false);
                GW_LOGW("NET", "Ethernet link DOWN");
            }
        }

        if (active_cfg.dhcp && link && !s_status.ip_ready && !ip4_addr_isany_val(*netif_ip4_addr(&s_netif))) {
            s_status.ipv4_addr=ip4_addr_get_u32(netif_ip4_addr(&s_netif));set_ip_event(true);log_ipv4("DHCP ready");
        }
        gw_runtime_config_t requested_cfg;gw_config_get_runtime(&requested_cfg);
        apply_runtime_network(&active_cfg,&requested_cfg);active_cfg=requested_cfg;
#if (GW_ETH_DIAGNOSTIC_LOG != 0U)
        if ((int32_t)(xTaskGetTickCount() - next_diag) >= 0) {
            gw_ethernetif_stats_t es;
            memset(&es, 0, sizeof(es));
            gw_ethernetif_get_stats(&es);
            GW_LOGI("NET", "ETH link=%s ip=%s rxIRQ=%lu rx=%lu rxAlloc=%lu rxDrop=%lu tx=%lu txFail=%lu txWait=%lu",
                    s_status.link_up ? "UP" : "DOWN",
                    s_status.ip_ready ? "READY" : "WAIT",
                    (unsigned long)es.rx_irq,
                    (unsigned long)es.rx_frames,
                    (unsigned long)es.rx_alloc_fail,
                    (unsigned long)es.rx_input_fail,
                    (unsigned long)es.tx_frames,
                    (unsigned long)es.tx_fail,
                    (unsigned long)es.tx_busy_timeout);
            next_diag = xTaskGetTickCount() + pdMS_TO_TICKS(GW_ETH_DIAG_PERIOD_MS);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(NET_LINK_POLL_MS));
    }
}

void gw_net_task_create(void)
{
    TaskHandle_t h = xTaskCreateStatic(network_task, "net", NET_TASK_STACK_WORDS,
                                       NULL, NET_TASK_PRIORITY,
                                       s_net_stack, &s_net_tcb);
    configASSERT(h != NULL);
}

void gw_net_get_status(gw_net_status_t *out)
{
    if (out != NULL) {
        taskENTER_CRITICAL();
        *out = s_status;
        taskEXIT_CRITICAL();
    }
}

#else
void gw_net_task_create(void) {}
void gw_net_get_status(gw_net_status_t *out) { if (out) memset(out, 0, sizeof(*out)); }
#endif
