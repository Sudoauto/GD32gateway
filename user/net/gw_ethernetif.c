#include "gw_ethernetif.h"
#include <string.h>
#include "gw_eth_port.h"
#include "gateway_build_config.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "netif/etharp.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "gd32h7xx_enet.h"

#if (GW_ETH_ENABLE != 0U)

extern enet_descriptors_struct rxdesc_tab[ENET_RXBUF_NUM];
extern enet_descriptors_struct txdesc_tab[ENET_TXBUF_NUM];
extern enet_descriptors_struct *dma_current_txdesc;
extern enet_descriptors_struct *dma_current_rxdesc;

static struct netif *s_netif;
static SemaphoreHandle_t s_rx_sem;
static SemaphoreHandle_t s_tx_mutex;
static gw_ethernetif_stats_t s_stats;

#define ETH_INPUT_STACK_WORDS   768U
#define ETH_INPUT_PRIORITY      4U
#define ETH_TX_WAIT_MS          100U
#define ETH_RX_BURST_MAX        8U

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    if ((p == NULL) || (s_tx_mutex == NULL)) {
        return ERR_ARG;
    }
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(ETH_TX_WAIT_MS)) != pdTRUE) {
        ++s_stats.tx_busy_timeout;
        return ERR_TIMEOUT;
    }

    TickType_t start = xTaskGetTickCount();
    while ((dma_current_txdesc->status & ENET_TDES0_DAV) != 0U) {
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(ETH_TX_WAIT_MS)) {
            ++s_stats.tx_busy_timeout;
            (void)xSemaphoreGive(s_tx_mutex);
            return ERR_TIMEOUT;
        }
        taskYIELD();
    }

    uint8_t *buffer = (uint8_t *)(uintptr_t)enet_desc_information_get(
        ENET0, dma_current_txdesc, TXDESC_BUFFER_1_ADDR);
    uint32_t frame_len = 0U;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if ((frame_len + q->len) > ENET_TXBUF_SIZE) {
            ++s_stats.tx_fail;
            (void)xSemaphoreGive(s_tx_mutex);
            return ERR_BUF;
        }
        memcpy(&buffer[frame_len], q->payload, q->len);
        frame_len += q->len;
    }

    ErrStatus st = ENET_NOCOPY_FRAME_TRANSMIT(ENET0, frame_len);
    if (st == SUCCESS) {
        ++s_stats.tx_frames;
    } else {
        ++s_stats.tx_fail;
    }
    (void)xSemaphoreGive(s_tx_mutex);
    return (st == SUCCESS) ? ERR_OK : ERR_IF;
}

static struct pbuf *low_level_input(bool *consumed)
{
    if (consumed == NULL) {
        return NULL;
    }
    *consumed = false;

    /* DMA still owns the current descriptor: no additional queued frame. */
    if ((dma_current_rxdesc->status & ENET_RDES0_DAV) != 0U) {
        return NULL;
    }
    *consumed = true;

    uint32_t status = dma_current_rxdesc->status;
    bool valid = ((status & ENET_RDES0_ERRS) == 0U) &&
                 ((status & ENET_RDES0_FDES) != 0U) &&
                 ((status & ENET_RDES0_LDES) != 0U);
    uint16_t len = 0U;
    if (valid) {
        /* Use the SPL helper instead of open-coding FRML/FCS handling. It
         * accounts for the MAC type-frame CRC-forwarding configuration. */
        uint32_t frame_len = enet_desc_information_get(
            ENET0, dma_current_rxdesc, RXDESC_FRAME_LENGTH);
        if ((frame_len > 0U) && (frame_len <= ENET_RXBUF_SIZE)) {
            len = (uint16_t)frame_len;
        } else {
            valid = false;
        }
    }

    uint8_t *buffer = (uint8_t *)(uintptr_t)dma_current_rxdesc->buffer1_addr;
    struct pbuf *p = NULL;
    if (valid && (len > 0U)) {
        p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        if (p != NULL) {
            uint32_t offset = 0U;
            for (struct pbuf *q = p; q != NULL; q = q->next) {
                memcpy(q->payload, &buffer[offset], q->len);
                offset += q->len;
            }
            ++s_stats.rx_frames;
        } else {
            ++s_stats.rx_alloc_fail;
        }
    } else {
        ++s_stats.rx_input_fail;
    }

    (void)ENET_NOCOPY_FRAME_RECEIVE(ENET0);
    return p;
}

static void ethernet_input_task(void *argument)
{
    (void)argument;
    for (;;) {
        if (xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(250U)) != pdTRUE) {
            continue;
        }

        for (uint32_t burst = 0U; burst < ETH_RX_BURST_MAX; ++burst) {
            bool consumed = false;
            struct pbuf *p = low_level_input(&consumed);
            if (!consumed) {
                break;
            }
            /* An invalid frame or temporary pbuf allocation failure still
             * consumes/releases one DMA descriptor. Continue draining the ring
             * so a bad frame cannot strand a valid frame behind it. */
            if (p == NULL) {
                continue;
            }
            if ((s_netif == NULL) || (s_netif->input(p, s_netif) != ERR_OK)) {
                ++s_stats.rx_input_fail;
                pbuf_free(p);
            }
        }
        taskYIELD();
    }
}

err_t gw_ethernetif_init(struct netif *netif)
{
    if (netif == NULL) {
        return ERR_ARG;
    }
    memset(&s_stats, 0, sizeof(s_stats));
    s_netif = netif;

    netif->hwaddr_len = ETH_HWADDR_LEN;
    netif->hwaddr[0] = GW_ETH_MAC0;
    netif->hwaddr[1] = GW_ETH_MAC1;
    netif->hwaddr[2] = GW_ETH_MAC2;
    netif->hwaddr[3] = GW_ETH_MAC3;
    netif->hwaddr[4] = GW_ETH_MAC4;
    netif->hwaddr[5] = GW_ETH_MAC5;
    netif->mtu = 1500U;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    netif->name[0] = 'g';
    netif->name[1] = 'w';
#if LWIP_NETIF_HOSTNAME
    netif->hostname = "gd32-gateway";
#endif
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;

    s_rx_sem = xSemaphoreCreateBinary();
    if (s_rx_sem == NULL) {
        return ERR_MEM;
    }
    s_tx_mutex = xSemaphoreCreateMutex();
    if (s_tx_mutex == NULL) {
        vSemaphoreDelete(s_rx_sem);
        s_rx_sem = NULL;
        return ERR_MEM;
    }

    enet_mac_address_set(ENET0, ENET_MAC_ADDRESS0, netif->hwaddr);
    enet_descriptors_chain_init(ENET0, ENET_DMA_TX);
    enet_descriptors_chain_init(ENET0, ENET_DMA_RX);

    for (uint32_t i = 0U; i < ENET_RXBUF_NUM; ++i) {
        enet_rx_desc_immediate_receive_complete_interrupt(&rxdesc_tab[i]);
    }
    for (uint32_t i = 0U; i < ENET_TXBUF_NUM; ++i) {
        enet_transmit_checksum_config(&txdesc_tab[i], ENET_CHECKSUM_TCPUDPICMP_FULL);
    }

    TaskHandle_t h = NULL;
    if (xTaskCreate(ethernet_input_task, "eth-rx", ETH_INPUT_STACK_WORDS,
                    NULL, ETH_INPUT_PRIORITY, &h) != pdPASS) {
        vSemaphoreDelete(s_tx_mutex);
        vSemaphoreDelete(s_rx_sem);
        s_tx_mutex = NULL;
        s_rx_sem = NULL;
        return ERR_MEM;
    }

    enet_enable(ENET0);
    gw_eth_port_irq_enable();
    return ERR_OK;
}

void gw_ethernetif_isr(void)
{
    BaseType_t hpw = pdFALSE;
    if (SET == enet_interrupt_flag_get(ENET0, ENET_DMA_INT_FLAG_RS)) {
        ++s_stats.rx_irq;
        if (s_rx_sem != NULL) {
            (void)xSemaphoreGiveFromISR(s_rx_sem, &hpw);
        }
    }
    enet_interrupt_flag_clear(ENET0, ENET_DMA_INT_FLAG_RS_CLR);
    enet_interrupt_flag_clear(ENET0, ENET_DMA_INT_FLAG_NI_CLR);
    portYIELD_FROM_ISR(hpw);
}

void gw_ethernetif_get_stats(gw_ethernetif_stats_t *out)
{
    if (out != NULL) {
        taskENTER_CRITICAL();
        *out = s_stats;
        taskEXIT_CRITICAL();
    }
}

#else
err_t gw_ethernetif_init(struct netif *netif) { (void)netif; return ERR_IF; }
void gw_ethernetif_isr(void) {}
void gw_ethernetif_get_stats(gw_ethernetif_stats_t *out) { if (out) memset(out, 0, sizeof(*out)); }
#endif
