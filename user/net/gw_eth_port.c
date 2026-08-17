#include "gw_eth_port.h"
#include "gateway_build_config.h"
#include "gw_log.h"
#include "gd32h7xx.h"
#include "FreeRTOS.h"
#include "task.h"

#if (GW_ETH_ENABLE != 0U)

static void rmii_gpio_config(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOF);
    rcu_periph_clock_enable(RCU_GPIOG);
    rcu_periph_clock_enable(RCU_SYSCFG);

#if (GW_ETH_RMII_REFCLK_PA8_MCO != 0U)
    /* Headless Ethernet mode: SYSCLK PLL0P 600 MHz / 12 -> PA8 CKOUT0 = 50 MHz.
     * Do not use this while the RGB LCD is enabled because PA8 is TLI_R6. */
    rcu_ckout0_config(RCU_CKOUT0SRC_PLL0P, RCU_CKOUT0_DIV12);
#else
    /* GUI mode: LAN8720A must receive an external 50 MHz RMII reference.
     * PA8 is intentionally left untouched here for the TLI LCD red channel. */
#endif
    syscfg_enet_phy_interface_config(ENET0, SYSCFG_ENET_PHY_RMII);

    /* PA1 REF_CLK, PA2 MDIO, PA7 CRS_DV */
    gpio_af_set(GPIOA, GPIO_AF_11, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ,
                            GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);

    /* PB11 TX_EN */
    gpio_af_set(GPIOB, GPIO_AF_11, GPIO_PIN_11);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_11);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ,
                            GPIO_PIN_11);

    /* PC1 MDC, PC4 RXD0, PC5 RXD1 */
    gpio_af_set(GPIOC, GPIO_AF_11, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ,
                            GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);

    /* PG13 TXD0, PG14 TXD1 */
    gpio_af_set(GPIOG, GPIO_AF_11, GPIO_PIN_13 | GPIO_PIN_14);
    gpio_mode_set(GPIOG, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_13 | GPIO_PIN_14);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ,
                            GPIO_PIN_13 | GPIO_PIN_14);

    /* PF6 PHY reset. */
    gpio_mode_set(GPIOF, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, GPIO_PIN_6);
    gpio_output_options_set(GPIOF, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ,
                            GPIO_PIN_6);
}

static void phy_hardware_reset(void)
{
    gpio_bit_reset(GPIOF, GPIO_PIN_6);
    vTaskDelay(pdMS_TO_TICKS(10U));
    gpio_bit_set(GPIOF, GPIO_PIN_6);
    vTaskDelay(pdMS_TO_TICKS(50U));
}

static bool phy_read(uint16_t reg, uint16_t *value)
{
    if (value == NULL) {
        return false;
    }
    return SUCCESS == enet_phy_write_read(ENET0, ENET_PHY_READ,
                                           (uint16_t)GW_ETH_PHY_ADDRESS,
                                           reg, value);
}

bool gw_eth_port_prepare(void)
{
    rmii_gpio_config();
    phy_hardware_reset();

    rcu_periph_clock_enable(RCU_ENET0);
    rcu_periph_clock_enable(RCU_ENET0TX);
    rcu_periph_clock_enable(RCU_ENET0RX);

    enet_deinit(ENET0);
    if (SUCCESS != enet_software_reset(ENET0)) {
        GW_LOGE("ETH", "ENET0 software reset failed");
        return false;
    }

    /* Configure SMI MDC and reset the PHY. Unlike enet_init(AUTO_NEGOTIATION),
     * this does not require the cable to be connected, so boot remains healthy
     * when Ethernet is unplugged. */
    if (SUCCESS != enet_phy_config(ENET0)) {
        GW_LOGE("ETH", "PHY/SMI init failed (addr=%u)",
                (unsigned)GW_ETH_PHY_ADDRESS);
        return false;
    }
    return true;
}

bool gw_eth_port_link_up(void)
{
    uint16_t bsr = 0U;
    /* BMSR link is latch-low; read twice so the second read is current. */
    if (!phy_read(PHY_REG_BSR, &bsr) || !phy_read(PHY_REG_BSR, &bsr)) {
        return false;
    }
    return (bsr & PHY_LINKED_STATUS) != 0U;
}

bool gw_eth_port_mac_init(void)
{
    /* The supplied LAN8720A example uses auto-negotiation and hardware checksum
     * offload. Call only after link is detected because the SPL auto-negotiation
     * routine intentionally waits for PHY_LINKED_STATUS. */
    if (SUCCESS != enet_init(ENET0, ENET_AUTO_NEGOTIATION,
                             ENET_AUTOCHECKSUM_DROP_FAILFRAMES,
                             ENET_BROADCAST_FRAMES_PASS)) {
        return false;
    }
    return true;
}

void gw_eth_port_update_mac_from_phy(void)
{
    uint16_t phy_sr = 0U;
    if (!phy_read(PHY_SR, &phy_sr)) {
        return;
    }

    uint32_t cfg = ENET_MAC_CFG(ENET0);
    cfg &= ~(ENET_MAC_CFG_SPD | ENET_MAC_CFG_DPM);
    if ((phy_sr & PHY_SPEED_STATUS) != 0U) {
        cfg |= ENET_SPEEDMODE_10M;
    } else {
        cfg |= ENET_SPEEDMODE_100M;
    }
    if ((phy_sr & PHY_DUPLEX_STATUS) != 0U) {
        cfg |= ENET_MODE_FULLDUPLEX;
    } else {
        cfg |= ENET_MODE_HALFDUPLEX;
    }
    ENET_MAC_CFG(ENET0) = cfg;
}

bool gw_eth_port_get_link_mode(bool *speed_100m, bool *full_duplex)
{
    if ((speed_100m == NULL) || (full_duplex == NULL)) {
        return false;
    }
    uint16_t phy_sr = 0U;
    if (!phy_read(PHY_SR, &phy_sr)) {
        return false;
    }
    *speed_100m = ((phy_sr & PHY_SPEED_STATUS) == 0U);
    *full_duplex = ((phy_sr & PHY_DUPLEX_STATUS) != 0U);
    return true;
}

void gw_eth_port_irq_enable(void)
{
    enet_interrupt_flag_clear(ENET0, ENET_DMA_INT_FLAG_RS_CLR);
    enet_interrupt_flag_clear(ENET0, ENET_DMA_INT_FLAG_NI_CLR);
    enet_interrupt_enable(ENET0, ENET_DMA_INT_NIE);
    enet_interrupt_enable(ENET0, ENET_DMA_INT_RIE);
    nvic_irq_enable(ENET0_IRQn, GW_ETH_IRQ_PREEMPT_PRIORITY, 0U);
}

void gw_eth_port_irq_disable(void)
{
    nvic_irq_disable(ENET0_IRQn);
    enet_interrupt_disable(ENET0, ENET_DMA_INT_RIE);
    enet_interrupt_disable(ENET0, ENET_DMA_INT_NIE);
}

uint16_t gw_eth_port_phy_id1(void)
{
    uint16_t v = 0U;
    (void)phy_read(2U, &v);
    return v;
}

uint16_t gw_eth_port_phy_id2(void)
{
    uint16_t v = 0U;
    (void)phy_read(3U, &v);
    return v;
}

bool gw_eth_port_phy_identity_ok(void)
{
    /* LAN8720A: PHYID1=0x0007, PHYID2[15:4]=0xC0F; low nibble is revision. */
    uint16_t id1 = gw_eth_port_phy_id1();
    uint16_t id2 = gw_eth_port_phy_id2();
    return (id1 == 0x0007U) && ((id2 >> 4U) == 0x0C0FU);
}

#else
bool gw_eth_port_prepare(void) { return false; }
bool gw_eth_port_link_up(void) { return false; }
bool gw_eth_port_mac_init(void) { return false; }
void gw_eth_port_update_mac_from_phy(void) {}
bool gw_eth_port_get_link_mode(bool *speed_100m, bool *full_duplex) { (void)speed_100m; (void)full_duplex; return false; }
void gw_eth_port_irq_enable(void) {}
void gw_eth_port_irq_disable(void) {}
uint16_t gw_eth_port_phy_id1(void) { return 0U; }
uint16_t gw_eth_port_phy_id2(void) { return 0U; }
bool gw_eth_port_phy_identity_ok(void) { return false; }
#endif
