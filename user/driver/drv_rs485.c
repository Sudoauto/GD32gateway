/*!
    \file    drv_rs485.c
    \brief   UART4 RS485 DMA driver
*/

#include "drv_rs485.h"
#include <stddef.h>
#include <string.h>
#include "gd32h7xx.h"
#include "bsp_cache.h"
#include "gateway_build_config.h"

#define UART_PERIPH             GW_RS485_UART
#define UART_CLK                GW_RS485_UART_RCU
#define GPIO_CLK                GW_RS485_GPIO_RCU
#define GPIO_PORT               GW_RS485_GPIO_PORT
#define TX_PIN                  GW_RS485_TX_PIN
#define RX_PIN                  GW_RS485_RX_PIN
#define GPIO_AF                 GW_RS485_GPIO_AF
#define DE_PORT                 GW_RS485_DE_PORT
#define DE_PIN                  GW_RS485_DE_PIN

#define DMA_PERIPH              GW_RS485_DMA
#define DMA_CLK                 GW_RS485_DMA_RCU
#define TX_DMA_CH               GW_RS485_TX_DMA_CH
#define RX_DMA_CH               GW_RS485_RX_DMA_CH
#define TX_DMA_REQ              GW_RS485_TX_DMA_REQUEST
#define RX_DMA_REQ              GW_RS485_RX_DMA_REQUEST

#define UART_IRQn               GW_RS485_UART_IRQn
#define TX_DMA_IRQn             GW_RS485_TX_DMA_IRQn
#define RX_DMA_IRQn             GW_RS485_RX_DMA_IRQn

#define UART_TDATA_ADDR         ((uint32_t)(uintptr_t)(&USART_TDATA(UART_PERIPH)))
#define UART_RDATA_ADDR         ((uint32_t)(uintptr_t)(&USART_RDATA(UART_PERIPH)))

static __attribute__((aligned(32))) uint8_t s_tx_buf[RS485_TX_DMA_BUF_SIZE];
static __attribute__((aligned(32))) uint8_t s_rx_buf[RS485_RX_DMA_BUF_SIZE];

static volatile bool s_tx_busy;
static volatile bool s_rx_armed;
static volatile uint16_t s_rx_snapshot_len;
static volatile uint16_t s_rx_dma_limit;
static TaskHandle_t s_task_handle;
static rs485_dma_stats_t s_stats;

static void de_set_rx(void)
{
    gpio_bit_reset(DE_PORT, DE_PIN);
}

static void de_set_tx(void)
{
    gpio_bit_set(DE_PORT, DE_PIN);
}

static bool dma_ch_has_error(uint32_t dma, dma_channel_enum ch)
{
    if (RESET != dma_interrupt_flag_get(dma, ch, DMA_INT_FLAG_FEE)) return true;
    if (RESET != dma_interrupt_flag_get(dma, ch, DMA_INT_FLAG_SDE)) return true;
    if (RESET != dma_interrupt_flag_get(dma, ch, DMA_INT_FLAG_TAE)) return true;
    return false;
}

static void dma_ch_clear_all_flags(uint32_t dma, dma_channel_enum ch)
{
    dma_interrupt_flag_clear(dma, ch, DMA_INT_FLAG_FTF);
    dma_interrupt_flag_clear(dma, ch, DMA_INT_FLAG_HTF);
    dma_interrupt_flag_clear(dma, ch, DMA_INT_FLAG_FEE);
    dma_interrupt_flag_clear(dma, ch, DMA_INT_FLAG_SDE);
    dma_interrupt_flag_clear(dma, ch, DMA_INT_FLAG_TAE);
}

static void notify_from_isr(uint32_t bits, BaseType_t *px_hpw)
{
    if (s_task_handle != NULL) {
        (void)xTaskNotifyFromISR(s_task_handle, bits, eSetBits, px_hpw);
    }
}

static void rx_dma_stop(void)
{
    usart_dma_receive_config(UART_PERIPH, USART_RECEIVE_DMA_DISABLE);
    dma_channel_disable(DMA_PERIPH, RX_DMA_CH);
    s_rx_armed = false;
}

/* Stop new UART RX DMA requests before sampling CHCNT.  Reading CHCNT after
 * disabling the DMA channel is avoided deliberately: the count we need is the
 * live remaining-transfer value belonging to this receive window. */
static uint32_t rx_dma_freeze_remaining(void)
{
    usart_dma_receive_config(UART_PERIPH, USART_RECEIVE_DMA_DISABLE);

    /* An already accepted UART request can still retire after DENR is cleared.
     * Sample until the counter is stable before disabling the DMA channel. */
    uint32_t remain = dma_transfer_number_get(DMA_PERIPH, RX_DMA_CH);
    for (uint32_t i = 0U; i < 4U; ++i) {
        uint32_t current = dma_transfer_number_get(DMA_PERIPH, RX_DMA_CH);
        if (current == remain) {
            break;
        }
        remain = current;
    }

    dma_channel_disable(DMA_PERIPH, RX_DMA_CH);
    s_rx_armed = false;
    return remain;
}

void drv_rs485_set_tx(void)
{
    de_set_tx();
}

void drv_rs485_set_rx(void)
{
    de_set_rx();
}

void drv_rs485_set_task_handle(TaskHandle_t handle)
{
    s_task_handle = handle;
}

bool drv_rs485_init(const rs485_config_t *cfg)
{
    if ((cfg == NULL) || (cfg->baudrate == 0U) ||
        (cfg->data_bits != 8U) ||
        ((cfg->stop_bits != 1U) && (cfg->stop_bits != 2U)) ||
        (cfg->parity > RS485_PARITY_ODD)) {
        return false;
    }

    rcu_periph_clock_enable(GPIO_CLK);
    rcu_periph_clock_enable(UART_CLK);

    gpio_mode_set(DE_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, DE_PIN);
    gpio_output_options_set(DE_PORT, GPIO_OTYPE_PP,
                            GPIO_OSPEED_100_220MHZ, DE_PIN);
    de_set_rx();

    gpio_af_set(GPIO_PORT, GPIO_AF, TX_PIN | RX_PIN);
    gpio_mode_set(GPIO_PORT, GPIO_MODE_AF, GPIO_PUPD_PULLUP, TX_PIN | RX_PIN);
    gpio_output_options_set(GPIO_PORT, GPIO_OTYPE_PP,
                            GPIO_OSPEED_100_220MHZ, TX_PIN | RX_PIN);

    usart_deinit(UART_PERIPH);
    usart_baudrate_set(UART_PERIPH, cfg->baudrate);

    /* GD32 word length includes the parity bit. */
    if (cfg->parity == RS485_PARITY_NONE) {
        usart_word_length_set(UART_PERIPH, USART_WL_8BIT);
        usart_parity_config(UART_PERIPH, USART_PM_NONE);
    } else {
        usart_word_length_set(UART_PERIPH, USART_WL_9BIT);
        usart_parity_config(UART_PERIPH,
            (cfg->parity == RS485_PARITY_EVEN) ? USART_PM_EVEN : USART_PM_ODD);
    }

    usart_stop_bit_set(UART_PERIPH,
        (cfg->stop_bits == 2U) ? USART_STB_2BIT : USART_STB_1BIT);
    usart_receive_config(UART_PERIPH, USART_RECEIVE_ENABLE);
    usart_transmit_config(UART_PERIPH, USART_TRANSMIT_ENABLE);
    usart_enable(UART_PERIPH);

    s_tx_busy = false;
    s_rx_armed = false;
    s_rx_snapshot_len = 0U;
    s_rx_dma_limit = RS485_RX_DMA_BUF_SIZE;
    memset(&s_stats, 0, sizeof(s_stats));
    return true;
}

bool drv_rs485_dma_init(void)
{
    rcu_periph_clock_enable(DMA_CLK);
    rcu_periph_clock_enable(RCU_DMAMUX);

    {
        dma_single_data_parameter_struct cfg;
        dma_deinit(DMA_PERIPH, TX_DMA_CH);
        dma_single_data_para_struct_init(&cfg);
        cfg.request = TX_DMA_REQ;
        cfg.direction = DMA_MEMORY_TO_PERIPH;
        cfg.memory0_addr = (uint32_t)(uintptr_t)s_tx_buf;
        cfg.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
        cfg.periph_memory_width = DMA_PERIPH_WIDTH_8BIT;
        cfg.number = 1U;
        cfg.periph_addr = UART_TDATA_ADDR;
        cfg.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
        cfg.priority = DMA_PRIORITY_ULTRA_HIGH;
        dma_single_data_mode_init(DMA_PERIPH, TX_DMA_CH, &cfg);
        dma_circulation_disable(DMA_PERIPH, TX_DMA_CH);
        dma_interrupt_enable(DMA_PERIPH, TX_DMA_CH,
                             DMA_INT_FTF | DMA_INT_TAE | DMA_INT_SDE | DMA_INT_FEE);
    }

    {
        dma_single_data_parameter_struct cfg;
        dma_deinit(DMA_PERIPH, RX_DMA_CH);
        dma_single_data_para_struct_init(&cfg);
        cfg.request = RX_DMA_REQ;
        cfg.direction = DMA_PERIPH_TO_MEMORY;
        cfg.memory0_addr = (uint32_t)(uintptr_t)s_rx_buf;
        cfg.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
        cfg.periph_memory_width = DMA_PERIPH_WIDTH_8BIT;
        cfg.number = RS485_RX_DMA_BUF_SIZE;
        cfg.periph_addr = UART_RDATA_ADDR;
        cfg.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
        cfg.priority = DMA_PRIORITY_ULTRA_HIGH;
        dma_single_data_mode_init(DMA_PERIPH, RX_DMA_CH, &cfg);
        dma_circulation_disable(DMA_PERIPH, RX_DMA_CH);
        dma_interrupt_enable(DMA_PERIPH, RX_DMA_CH,
                             DMA_INT_FTF | DMA_INT_TAE | DMA_INT_SDE | DMA_INT_FEE);
    }

    nvic_irq_enable(UART_IRQn, GW_RTOS_IRQ_PREEMPT_PRIORITY,
                    GW_RTOS_IRQ_SUB_PRIORITY);
    nvic_irq_enable(TX_DMA_IRQn, GW_RTOS_IRQ_PREEMPT_PRIORITY,
                    GW_RTOS_IRQ_SUB_PRIORITY);
    nvic_irq_enable(RX_DMA_IRQn, GW_RTOS_IRQ_PREEMPT_PRIORITY,
                    GW_RTOS_IRQ_SUB_PRIORITY);

    usart_flag_clear(UART_PERIPH, USART_FLAG_IDLE);
    usart_flag_clear(UART_PERIPH, USART_FLAG_TC);
    usart_flag_clear(UART_PERIPH, USART_FLAG_ORERR);
    usart_flag_clear(UART_PERIPH, USART_FLAG_NERR);
    usart_flag_clear(UART_PERIPH, USART_FLAG_FERR);
    usart_flag_clear(UART_PERIPH, USART_FLAG_PERR);
    usart_interrupt_enable(UART_PERIPH, USART_INT_IDLE);
    usart_interrupt_enable(UART_PERIPH, USART_INT_ERR);

    return true;
}

void drv_rs485_rx_arm(uint16_t max_len)
{
    rx_dma_stop();
    dma_ch_clear_all_flags(DMA_PERIPH, RX_DMA_CH);

    if ((max_len == 0U) || (max_len > RS485_RX_DMA_BUF_SIZE)) {
        max_len = RS485_RX_DMA_BUF_SIZE;
    }
    s_rx_dma_limit = max_len;

    /* Buffer is 32-byte aligned and 256 bytes long, so invalidation cannot
     * evict unrelated objects sharing a cache line. */
    bsp_dcache_invalidate(s_rx_buf, s_rx_dma_limit);
    s_rx_snapshot_len = 0U;
    dma_memory_address_config(DMA_PERIPH, RX_DMA_CH, DMA_MEMORY_0,
                              (uint32_t)(uintptr_t)s_rx_buf);
    dma_transfer_number_config(DMA_PERIPH, RX_DMA_CH, s_rx_dma_limit);

    usart_flag_clear(UART_PERIPH, USART_FLAG_IDLE);
    usart_flag_clear(UART_PERIPH, USART_FLAG_ORERR);
    usart_flag_clear(UART_PERIPH, USART_FLAG_NERR);
    usart_flag_clear(UART_PERIPH, USART_FLAG_FERR);
    usart_flag_clear(UART_PERIPH, USART_FLAG_PERR);

    s_rx_armed = true;
    dma_channel_enable(DMA_PERIPH, RX_DMA_CH);
    usart_dma_receive_config(UART_PERIPH, USART_RECEIVE_DMA_ENABLE);
}

bool drv_rs485_rx_resume(void)
{
    uint16_t offset = s_rx_snapshot_len;
    if (s_rx_armed || (offset >= s_rx_dma_limit)) {
        return false;
    }

    rx_dma_stop();
    dma_ch_clear_all_flags(DMA_PERIPH, RX_DMA_CH);

    uint16_t remaining = (uint16_t)(s_rx_dma_limit - offset);
    bsp_dcache_invalidate(&s_rx_buf[offset], remaining);
    dma_memory_address_config(DMA_PERIPH, RX_DMA_CH, DMA_MEMORY_0,
                              (uint32_t)(uintptr_t)&s_rx_buf[offset]);
    dma_transfer_number_config(DMA_PERIPH, RX_DMA_CH, remaining);

    usart_flag_clear(UART_PERIPH, USART_FLAG_IDLE);
    usart_flag_clear(UART_PERIPH, USART_FLAG_ORERR);
    usart_flag_clear(UART_PERIPH, USART_FLAG_NERR);
    usart_flag_clear(UART_PERIPH, USART_FLAG_FERR);
    usart_flag_clear(UART_PERIPH, USART_FLAG_PERR);

    s_rx_armed = true;
    dma_channel_enable(DMA_PERIPH, RX_DMA_CH);
    usart_dma_receive_config(UART_PERIPH, USART_RECEIVE_DMA_ENABLE);
    return true;
}

void drv_rs485_rx_abort(void)
{
    rx_dma_stop();
    dma_ch_clear_all_flags(DMA_PERIPH, RX_DMA_CH);
    s_rx_snapshot_len = 0U;
}

bool drv_rs485_tx_busy(void)
{
    return s_tx_busy;
}

bool drv_rs485_tx_dma_start(const uint8_t *data, uint16_t len)
{
    if ((data == NULL) || (len == 0U) ||
        (len > RS485_TX_DMA_BUF_SIZE) || s_tx_busy) {
        return false;
    }

    /* Keep the UART DMA request disabled until the channel is completely
     * programmed. This makes each attempt a fresh one-shot transfer and avoids
     * carrying a pending TBE request across retries/aborts. */
    usart_dma_transmit_config(UART_PERIPH, USART_TRANSMIT_DMA_DISABLE);
    dma_channel_disable(DMA_PERIPH, TX_DMA_CH);
    dma_ch_clear_all_flags(DMA_PERIPH, TX_DMA_CH);

    memcpy(s_tx_buf, data, len);
    bsp_dcache_clean(s_tx_buf, len);
    dma_memory_address_config(DMA_PERIPH, TX_DMA_CH, DMA_MEMORY_0,
                              (uint32_t)(uintptr_t)s_tx_buf);
    dma_transfer_number_config(DMA_PERIPH, TX_DMA_CH, (uint32_t)len);

    usart_interrupt_disable(UART_PERIPH, USART_INT_TC);
    usart_flag_clear(UART_PERIPH, USART_FLAG_TC);

    s_tx_busy = true;
    ++s_stats.tx_dma_start_count;
    s_stats.tx_bytes += len;
    de_set_tx();
    dma_channel_enable(DMA_PERIPH, TX_DMA_CH);
    usart_dma_transmit_config(UART_PERIPH, USART_TRANSMIT_DMA_ENABLE);
    return true;
}

void drv_rs485_abort_tx(void)
{
    dma_channel_disable(DMA_PERIPH, TX_DMA_CH);
    usart_dma_transmit_config(UART_PERIPH, USART_TRANSMIT_DMA_DISABLE);
    usart_interrupt_disable(UART_PERIPH, USART_INT_TC);
    dma_ch_clear_all_flags(DMA_PERIPH, TX_DMA_CH);
    de_set_rx();
    s_tx_busy = false;
}

uint16_t drv_rs485_rx_snapshot_len(void)
{
    return s_rx_snapshot_len;
}

uint16_t drv_rs485_rx_read(uint8_t *dst, uint16_t max_len)
{
    if ((dst == NULL) || (max_len == 0U) || s_rx_armed) {
        return 0U;
    }

    uint16_t len = s_rx_snapshot_len;
    if (len > s_rx_dma_limit) len = s_rx_dma_limit;
    if (len > RS485_RX_DMA_BUF_SIZE) len = RS485_RX_DMA_BUF_SIZE;
    if (len > max_len) len = max_len;
    if (len == 0U) return 0U;

    bsp_dcache_invalidate(s_rx_buf, len);
    memcpy(dst, s_rx_buf, len);
    s_stats.rx_bytes += len;
    return len;
}

void drv_rs485_get_stats(rs485_dma_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}

void drv_rs485_isr_tx_dma(void)
{
    BaseType_t hpw = pdFALSE;

    if (dma_ch_has_error(DMA_PERIPH, TX_DMA_CH)) {
        ++s_stats.dma_error_count;
        dma_ch_clear_all_flags(DMA_PERIPH, TX_DMA_CH);
        dma_channel_disable(DMA_PERIPH, TX_DMA_CH);
        usart_dma_transmit_config(UART_PERIPH, USART_TRANSMIT_DMA_DISABLE);
        usart_interrupt_disable(UART_PERIPH, USART_INT_TC);
        de_set_rx();
        s_tx_busy = false;
        notify_from_isr(RS485_NTF_UART_ERROR, &hpw);
        portYIELD_FROM_ISR(hpw);
        return;
    }

    if (RESET != dma_interrupt_flag_get(DMA_PERIPH, TX_DMA_CH,
                                        DMA_INT_FLAG_FTF)) {
        ++s_stats.tx_dma_ftf_count;
        dma_interrupt_flag_clear(DMA_PERIPH, TX_DMA_CH, DMA_INT_FLAG_FTF);
        usart_dma_transmit_config(UART_PERIPH, USART_TRANSMIT_DMA_DISABLE);
        dma_channel_disable(DMA_PERIPH, TX_DMA_CH);

        /* DMA complete only means TDATA accepted the final byte. If the ISR
         * was delayed long enough for TC to be set already, consume that state
         * directly. Never clear a real TC here or no future TC edge may occur. */
        if (RESET != usart_flag_get(UART_PERIPH, USART_FLAG_TC)) {
            ++s_stats.tx_tc_complete_count;
            de_set_rx();
            s_tx_busy = false;
            notify_from_isr(RS485_NTF_TX_DONE, &hpw);
        } else {
            usart_interrupt_enable(UART_PERIPH, USART_INT_TC);
        }
    }

    portYIELD_FROM_ISR(hpw);
}

void drv_rs485_isr_rx_dma(void)
{
    BaseType_t hpw = pdFALSE;

    if (dma_ch_has_error(DMA_PERIPH, RX_DMA_CH)) {
        ++s_stats.dma_error_count;
        dma_ch_clear_all_flags(DMA_PERIPH, RX_DMA_CH);
        rx_dma_stop();
        notify_from_isr(RS485_NTF_UART_ERROR, &hpw);
        portYIELD_FROM_ISR(hpw);
        return;
    }

    if (RESET != dma_interrupt_flag_get(DMA_PERIPH, RX_DMA_CH,
                                        DMA_INT_FLAG_FTF)) {
        dma_interrupt_flag_clear(DMA_PERIPH, RX_DMA_CH, DMA_INT_FLAG_FTF);

        /* A real RX full-transfer event must belong to an armed receive window
         * and the channel count must have reached zero.  This filters stale or
         * cross-channel flag observations seen during the M1/M2 board soak. */
        if (!s_rx_armed ||
            (dma_transfer_number_get(DMA_PERIPH, RX_DMA_CH) != 0U)) {
            ++s_stats.rx_dma_spurious_ftf_count;
        } else {
            ++s_stats.rx_dma_ftf_count;
            rx_dma_stop();
            s_rx_snapshot_len = s_rx_dma_limit;
            notify_from_isr(RS485_NTF_RX_EVENT, &hpw);
        }
    }

    portYIELD_FROM_ISR(hpw);
}

void drv_rs485_isr_uart(void)
{
    BaseType_t hpw = pdFALSE;

    if (RESET != usart_interrupt_flag_get(UART_PERIPH, USART_INT_FLAG_TC)) {
        ++s_stats.tx_tc_complete_count;
        usart_interrupt_disable(UART_PERIPH, USART_INT_TC);
        usart_flag_clear(UART_PERIPH, USART_FLAG_TC);
        de_set_rx();
        s_tx_busy = false;
        notify_from_isr(RS485_NTF_TX_DONE, &hpw);
    }

    if (RESET != usart_interrupt_flag_get(UART_PERIPH, USART_INT_FLAG_IDLE)) {
        ++s_stats.rx_idle_count;
        usart_flag_clear(UART_PERIPH, USART_FLAG_IDLE);

        if (s_rx_armed) {
            /* Stop new UART DMA requests, sample the live remaining count,
             * then disable the channel.  The programmed count may be smaller
             * than the physical 256-byte buffer for a known-length response. */
            uint32_t remain = rx_dma_freeze_remaining();
            if (remain > s_rx_dma_limit) {
                remain = s_rx_dma_limit;
            }
            uint32_t received = s_rx_dma_limit - remain;
            if (received > s_rx_dma_limit) {
                received = s_rx_dma_limit;
            }
            s_rx_snapshot_len = (uint16_t)received;
            if (received > 0U) {
                notify_from_isr(RS485_NTF_RX_EVENT, &hpw);
            }
        }
    }

    {
        bool err = false;
        if (RESET != usart_flag_get(UART_PERIPH, USART_FLAG_ORERR)) {
            usart_flag_clear(UART_PERIPH, USART_FLAG_ORERR); err = true;
        }
        if (RESET != usart_flag_get(UART_PERIPH, USART_FLAG_NERR)) {
            usart_flag_clear(UART_PERIPH, USART_FLAG_NERR); err = true;
        }
        if (RESET != usart_flag_get(UART_PERIPH, USART_FLAG_FERR)) {
            usart_flag_clear(UART_PERIPH, USART_FLAG_FERR); err = true;
        }
        if (RESET != usart_flag_get(UART_PERIPH, USART_FLAG_PERR)) {
            usart_flag_clear(UART_PERIPH, USART_FLAG_PERR); err = true;
        }
        if (err) {
            ++s_stats.uart_error_count;
            if (s_rx_armed) {
                rx_dma_stop();
            }
            notify_from_isr(RS485_NTF_UART_ERROR, &hpw);
        }
    }

    portYIELD_FROM_ISR(hpw);
}
