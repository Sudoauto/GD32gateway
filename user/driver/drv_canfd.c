#include "drv_canfd.h"
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "bsp_board.h"
#include "gateway_build_config.h"
#include "gd32h7xx.h"
#include "rtos_objects.h"

#define CAN_PERIPH             GW_CANFD_CAN
#define CAN_RCU                GW_CANFD_CAN_RCU
#define CAN_GPIO_PORT          GW_CANFD_GPIO_PORT
#define CAN_GPIO_RCU           GW_CANFD_GPIO_RCU
#define CAN_RX_PIN             GW_CANFD_RX_PIN
#define CAN_TX_PIN             GW_CANFD_TX_PIN
#define CAN_GPIO_AF            GW_CANFD_GPIO_AF

#define CAN_TX_MB              0U
#define CAN_RX_MB              1U

static canfd_stats_t s_stats;
static bool s_tx_active;
static volatile bool s_tx_hold;
static uint8_t s_tx_start_tec;
static uint8_t s_tx_start_fd_tec;
static uint8_t s_tx_active_len;

static bool valid_payload_length(const canfd_frame_t *frame)
{
    if (frame == NULL) {
        return false;
    }
    if (!frame->fd) {
        return (frame->len <= 8U) && !frame->brs && !frame->esi;
    }
#if (GW_CANFD_BRS_ENABLE == 0U)
    if (frame->brs) {
        return false;
    }
#endif
    if (frame->len <= 8U) {
        return true;
    }
    return (frame->len == 12U) || (frame->len == 16U) ||
           (frame->len == 20U) || (frame->len == 24U) ||
           (frame->len == 32U) || (frame->len == 48U) ||
           (frame->len == 64U);
}

static can_interrupt_enum irq_for_mailbox(uint32_t mb)
{
    if (mb == 1U) return CAN_INT_MB1;
    if (mb == 2U) return CAN_INT_MB2;
    return CAN_INT_MB0;
}

static can_interrupt_flag_enum flag_for_mailbox(uint32_t mb)
{
    if (mb == 1U) return CAN_INT_FLAG_MB1;
    if (mb == 2U) return CAN_INT_FLAG_MB2;
    return CAN_INT_FLAG_MB0;
}

static void error_counters_read(uint8_t *tec, uint8_t *fd_tec)
{
    can_error_counter_struct ec;
    can_struct_para_init(CAN_ERRCNT_STRUCT, &ec);
    can_error_counter_get(CAN_PERIPH, &ec);
    if (tec != NULL) {
        *tec = ec.tx_errcnt;
    }
    if (fd_tec != NULL) {
        *fd_tec = ec.fd_data_phase_tx_errcnt;
    }
}

static void tx_mailbox_quiesce(bool count_abort)
{
    /* MB0 is a level/pending interrupt source.  Always mask it before changing
     * the descriptor and acknowledge the pending source last.  In particular,
     * do not rely on s_tx_active to decide whether the hardware flag needs to
     * be cleared: an error/abort can make the software state inactive while
     * MB0 is still pending. */
    (void)can_interrupt_disable(CAN_PERIPH, irq_for_mailbox(CAN_TX_MB));

    uint32_t code = can_mailbox_code_get(CAN_PERIPH, CAN_TX_MB);
    bool was_active = s_tx_active || (code == CAN_MB_TX_STATUS_DATA);

    if (code == CAN_MB_TX_STATUS_DATA) {
        can_mailbox_transmit_abort(CAN_PERIPH, CAN_TX_MB);
        can_mailbox_transmit_inactive(CAN_PERIPH, CAN_TX_MB);
    } else if (code == CAN_MB_TX_STATUS_ABORT) {
        can_mailbox_transmit_inactive(CAN_PERIPH, CAN_TX_MB);
    } else if (s_tx_active) {
        /* Software thought a transmission was active but hardware is already
         * terminal. Force a known inactive state before acknowledging MB0. */
        can_mailbox_transmit_inactive(CAN_PERIPH, CAN_TX_MB);
    }

    s_tx_active = false;
    if (count_abort && was_active) {
        ++s_stats.tx_aborted;
    }

    /* Abort/inactive writes may themselves leave MB0 pending, so clear after
     * the mailbox has reached its final state. */
    can_interrupt_flag_clear(CAN_PERIPH, flag_for_mailbox(CAN_TX_MB));
}

#if (GW_CANFD_BRS_ENABLE != 0U)
static void tx_enter_hold(void)
{
    if (s_tx_hold) {
        return;
    }
    s_tx_hold = true;
    ++s_stats.tx_hold_count;
    tx_mailbox_quiesce(true);
    if (q_can_tx != NULL) {
        (void)xQueueReset(q_can_tx);
    }
}
#endif

static void rx_mailbox_arm(uint32_t mb)
{
    can_mailbox_descriptor_struct md;
    can_struct_para_init(CAN_MDSC_STRUCT, &md);
    /* Match the known-good board example: one wildcard receive mailbox.
     * CAN_IDE_RTR_FILTERED + public filter 0 makes IDE/RTR/ID don't-care;
     * the received descriptor still reports the actual IDE/RTR/ID. */
    md.rtr = 0U;
    md.ide = 0U;
    md.code = CAN_MB_RX_STATUS_EMPTY;
    md.id = 0U;
    can_mailbox_config(CAN_PERIPH, mb, &md);
}

static bool rx_mailbox_read_frame(uint32_t mb, canfd_frame_t *frame)
{
    if (frame == NULL) {
        return false;
    }

    uint32_t code = can_mailbox_code_get(CAN_PERIPH, mb);
    if ((code != CAN_MB_RX_STATUS_FULL) &&
        (code != CAN_MB_RX_STATUS_OVERRUN)) {
        return false;
    }

    if (code == CAN_MB_RX_STATUS_OVERRUN) {
        ++s_stats.rx_overrun;
    }

    uint32_t words[16] = {0U};
    can_mailbox_descriptor_struct md;
    can_struct_para_init(CAN_MDSC_STRUCT, &md);
    md.data = words;

    if (SUCCESS != can_mailbox_receive_data_read(CAN_PERIPH, mb, &md)) {
        ++s_stats.rx_read_error;
        /* SPL returns before its normal global-mailbox unlock when BUSY times
         * out.  Always unlock explicitly on this error path; otherwise GD32H7
         * erratum 2.15.4 allows CAN RAM corruption after a receive failure. */
        can_mailbox_receive_unlock(CAN_PERIPH);
        can_interrupt_flag_clear(CAN_PERIPH, flag_for_mailbox(mb));
        rx_mailbox_arm(mb);
        return false;
    }

    memset(frame, 0, sizeof(*frame));
    frame->id = md.id;
    frame->len = (md.data_bytes <= CANFD_MAX_DATA_BYTES) ?
                 (uint8_t)md.data_bytes : (uint8_t)CANFD_MAX_DATA_BYTES;
    frame->extended = (md.ide != 0U);
    frame->fd = (md.fdf != 0U);
    frame->brs = (md.brs != 0U);
    frame->esi = (md.esi != 0U);
    frame->timestamp = (uint16_t)md.timestamp;
    if (frame->len != 0U) {
        memcpy(frame->data, words, frame->len);
    }

    ++s_stats.rx_frames;
    s_stats.rx_bytes += frame->len;
    if (frame->fd) {
        ++s_stats.rx_fd_frames;
    }
    if (frame->fd && frame->brs) {
        ++s_stats.rx_brs_frames;
    }

    can_interrupt_flag_clear(CAN_PERIPH, flag_for_mailbox(mb));
    rx_mailbox_arm(mb);
    return true;
}

static void latch_error_flag(can_flag_enum flag, uint32_t *counter)
{
    if (SET == can_flag_get(CAN_PERIPH, flag)) {
        if (counter != NULL) {
            ++(*counter);
        }
        can_flag_clear(CAN_PERIPH, flag);
    }
}

static void handle_status_flags(void)
{
    can_error_counter_struct ec;
    can_struct_para_init(CAN_ERRCNT_STRUCT, &ec);
    can_error_counter_get(CAN_PERIPH, &ec);
    can_error_state_enum error_state = can_error_state_get(CAN_PERIPH);
    s_stats.error_state = (uint32_t)error_state;
    s_stats.rx_error_count = ec.rx_errcnt;
    s_stats.tx_error_count = ec.tx_errcnt;
    s_stats.fd_rx_error_count = ec.fd_data_phase_rx_errcnt;
    s_stats.fd_tx_error_count = ec.fd_data_phase_tx_errcnt;

    latch_error_flag(CAN_FLAG_ACK_ERR, &s_stats.ack_error_count);
    latch_error_flag(CAN_FLAG_STUFF_ERR, &s_stats.stuff_error_count);
    latch_error_flag(CAN_FLAG_FORM_ERR, &s_stats.form_error_count);
    latch_error_flag(CAN_FLAG_CRC_ERR, &s_stats.crc_error_count);
    latch_error_flag(CAN_FLAG_BIT_DOMINANT_ERR, &s_stats.bit_error_count);
    latch_error_flag(CAN_FLAG_BIT_RECESSIVE_ERR, &s_stats.bit_error_count);
    latch_error_flag(CAN_FLAG_STUFF_ERR_FD, &s_stats.fd_stuff_error_count);
    latch_error_flag(CAN_FLAG_FORM_ERR_FD, &s_stats.fd_form_error_count);
    latch_error_flag(CAN_FLAG_CRC_ERR_FD, &s_stats.fd_crc_error_count);
    latch_error_flag(CAN_FLAG_BIT_DOMINANT_ERR_FD, &s_stats.fd_bit_error_count);
    latch_error_flag(CAN_FLAG_BIT_RECESSIVE_ERR_FD, &s_stats.fd_bit_error_count);
#if (GW_CANFD_TDC_ACTIVE != 0U)
    if (SET == can_flag_get(CAN_PERIPH, CAN_FLAG_TDC_OUT_OF_RANGE)) {
        ++s_stats.tdc_out_of_range_count;
        can_flag_clear(CAN_PERIPH, CAN_FLAG_TDC_OUT_OF_RANGE);
    }
    s_stats.tdc_value = can_tdc_get(CAN_PERIPH);
#else
    s_stats.tdc_value = 0U;
    s_stats.tdc_out_of_range_count = 0U;
#endif

#if (GW_CANFD_BRS_ENABLE != 0U)
    if ((s_stats.tx_error_count >= GW_CANFD_TX_HOLD_TEC_THRESHOLD) ||
        (s_stats.fd_tx_error_count >= GW_CANFD_TX_HOLD_FD_TEC_THRESHOLD) ||
        (s_stats.error_state != (uint32_t)CAN_ERROR_STATE_ACTIVE)) {
        tx_enter_hold();
    }
#endif

    if (RESET != can_interrupt_flag_get(CAN_PERIPH, CAN_INT_FLAG_BUSOFF)) {
        ++s_stats.busoff_count;
        can_interrupt_flag_clear(CAN_PERIPH, CAN_INT_FLAG_BUSOFF);
        /* A bus-off can leave the TX mailbox interrupt pending even after the
         * transmit descriptor is aborted. Quiesce/ack it as one operation. */
        tx_mailbox_quiesce(true);
    }
    if (RESET != can_interrupt_flag_get(CAN_PERIPH,
                                         CAN_INT_FLAG_BUSOFF_RECOVERY)) {
        ++s_stats.busoff_recovery_count;
        /* GD32H7 erratum 2.15.5: TECNT is not guaranteed to clear after
         * automatic bus-off recovery.  Enter inactive mode, clear TECNT in
         * software, then return to normal operation before acknowledging the
         * recovery flag. */
        tx_mailbox_quiesce(true);
        if (SUCCESS == can_operation_mode_enter(CAN_PERIPH, CAN_INACTIVE_MODE)) {
            CAN_ERR0(CAN_PERIPH) &= ~CAN_ERR0_TECNT;
            (void)can_operation_mode_enter(CAN_PERIPH, CAN_NORMAL_MODE);
        }
        can_interrupt_flag_clear(CAN_PERIPH, CAN_INT_FLAG_BUSOFF_RECOVERY);
    }
    if (RESET != can_interrupt_flag_get(CAN_PERIPH,
                                         CAN_INT_FLAG_ERR_SUMMARY)) {
        ++s_stats.error_irq_count;
        can_interrupt_flag_clear(CAN_PERIPH, CAN_INT_FLAG_ERR_SUMMARY);
    }
    if (RESET != can_interrupt_flag_get(CAN_PERIPH,
                                         CAN_INT_FLAG_ERR_SUMMARY_FD)) {
        ++s_stats.fd_error_irq_count;
        can_interrupt_flag_clear(CAN_PERIPH, CAN_INT_FLAG_ERR_SUMMARY_FD);
    }
}

static void tx_completion_service(void)
{
    /* Acknowledge MB0 based on the hardware flag, not s_tx_active.  The old
     * implementation returned early when safe-hold had already cleared
     * s_tx_active, leaving MB0 pending; drv_canfd_service() then re-enabled the
     * source and created a CAN IRQ/can_task starvation loop. */
    if (RESET == can_interrupt_flag_get(CAN_PERIPH,
                                        flag_for_mailbox(CAN_TX_MB))) {
        return;
    }

    uint32_t code = can_mailbox_code_get(CAN_PERIPH, CAN_TX_MB);
    uint8_t tec_now = 0U;
    uint8_t fd_tec_now = 0U;
    error_counters_read(&tec_now, &fd_tec_now);

    /* Clear first once the descriptor snapshot is captured. No exit path below
     * is allowed to leave the level/pending MB0 source asserted. */
    can_interrupt_flag_clear(CAN_PERIPH,
                             flag_for_mailbox(CAN_TX_MB));

    if (!s_tx_active) {
        ++s_stats.tx_spurious_irq_count;
        /* Software/hardware state is inconsistent. Do not merely acknowledge
         * the flag: force MB0 to a known inactive state so tx_start_next() can
         * never overwrite a still-active hardware transmission. */
        tx_mailbox_quiesce(false);
        return;
    }

    if (code == CAN_MB_TX_STATUS_INACTIVE) {
        ++s_stats.tx_completed;
        /* MB0 becoming inactive is not, by itself, proof that the frame was
         * ACKed. Count a successful TX only when this attempt did not grow
         * either the nominal or FD-data-phase transmit error counter. */
        if ((tec_now <= s_tx_start_tec) &&
            (fd_tec_now <= s_tx_start_fd_tec)) {
            ++s_stats.tx_success;
            s_stats.tx_bytes += s_tx_active_len;
        } else {
            ++s_stats.tx_failed;
        }
    } else if (code == CAN_MB_TX_STATUS_ABORT) {
        ++s_stats.tx_aborted;
        can_mailbox_transmit_inactive(CAN_PERIPH, CAN_TX_MB);
    } else {
        ++s_stats.tx_failed;
        can_mailbox_transmit_abort(CAN_PERIPH, CAN_TX_MB);
        can_mailbox_transmit_inactive(CAN_PERIPH, CAN_TX_MB);
    }
    s_tx_active = false;

    /* ABORT -> INACTIVE transitions can assert MB0 again. Leave this service
     * routine with the source acknowledged in every terminal path. */
    can_interrupt_flag_clear(CAN_PERIPH,
                             flag_for_mailbox(CAN_TX_MB));
}

static void tx_start_next(void)
{
    if (s_tx_active || s_tx_hold) {
        return;
    }

    canfd_frame_t frame;
    if (xQueueReceive(q_can_tx, &frame, 0U) != pdTRUE) {
        return;
    }

    uint32_t words[16] = {0U};
    if (frame.len != 0U) {
        memcpy(words, frame.data, frame.len);
    }

    can_mailbox_descriptor_struct md;
    can_struct_para_init(CAN_MDSC_STRUCT, &md);
    md.code = CAN_MB_TX_STATUS_DATA;
    md.id = frame.id;
    md.ide = frame.extended ? 1U : 0U;
    md.fdf = frame.fd ? 1U : 0U;
    md.brs = (frame.fd && frame.brs) ? 1U : 0U;
    md.esi = frame.esi ? 1U : 0U;
    md.data = words;
    md.data_bytes = frame.len;
    error_counters_read(&s_tx_start_tec, &s_tx_start_fd_tec);
    can_mailbox_config(CAN_PERIPH, CAN_TX_MB, &md);
    s_tx_active_len = frame.len;
    s_tx_active = true;
    ++s_stats.tx_started;
}

bool drv_canfd_init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_tx_active = false;
    s_tx_hold = false;
    s_tx_start_tec = 0U;
    s_tx_start_fd_tec = 0U;
    s_tx_active_len = 0U;

    /* Match the known-good board example order: select CAN kernel clock first,
     * then enable the CAN and GPIO peripheral clocks. */
    rcu_can_clock_config(IDX_CAN2, RCU_CANSRC_APB2);
    rcu_periph_clock_enable(CAN_RCU);
    rcu_periph_clock_enable(CAN_GPIO_RCU);

    /* Start from the same clean peripheral state used by the board vendor example. */
    can_deinit(CAN_PERIPH);

    gpio_af_set(CAN_GPIO_PORT, CAN_GPIO_AF, CAN_RX_PIN | CAN_TX_PIN);
    gpio_mode_set(CAN_GPIO_PORT, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  CAN_RX_PIN | CAN_TX_PIN);
    gpio_output_options_set(CAN_GPIO_PORT, GPIO_OTYPE_PP,
                            GPIO_OSPEED_100_220MHZ,
                            CAN_RX_PIN | CAN_TX_PIN);

    can_parameter_struct init;
    can_struct_para_init(CAN_INIT_STRUCT, &init);
    init.internal_counter_source = CAN_TIMER_SOURCE_BIT_CLOCK;
    init.self_reception = (uint8_t)DISABLE;
    init.mb_tx_order = CAN_TX_HIGH_PRIORITY_MB_FIRST;
    init.mb_tx_abort_enable = (uint8_t)ENABLE;
    init.local_priority_enable = (uint8_t)DISABLE;
    init.mb_rx_ide_rtr_type = CAN_IDE_RTR_FILTERED;
    init.mb_remote_frame = CAN_STORE_REMOTE_REQUEST_FRAME;
    init.rx_private_filter_queue_enable = (uint8_t)DISABLE;
    init.edge_filter_enable = (uint32_t)DISABLE;
    init.protocol_exception_enable = (uint32_t)DISABLE;
    init.rx_filter_order = CAN_RX_FILTER_ORDER_MAILBOX_FIRST;
    /* Use the vendor-example bring-up setting. We only use MB0/MB1, but keeping
     * the full message RAM enabled removes RAM sizing as a test variable. */
    init.memory_size = CAN_MEMSIZE_32_UNIT;
    /* Match the vendor example: zero public mask + IDE/RTR filtered means the
     * single MB1 acts as a wildcard receive mailbox for standard/extended data. */
    init.mb_public_filter = 0x00000000U;

    /* CK_CAN=CK_APB2=300 MHz per GD32H7 erratum 2.15.7.
     * 300M / (60 * (1 + 2 + 5 + 2)) = 500 kbit/s, sample = 80%. */
    init.prescaler = GW_CANFD_NOMINAL_PRESCALER;
    init.resync_jump_width = GW_CANFD_NOMINAL_SJW;
    init.prop_time_segment = GW_CANFD_NOMINAL_PROP_SEG;
    init.time_segment_1 = GW_CANFD_NOMINAL_SEG1;
    init.time_segment_2 = GW_CANFD_NOMINAL_SEG2;

    if (SUCCESS != can_init(CAN_PERIPH, &init)) {
        return false;
    }

    can_fd_parameter_struct fd;
    can_struct_para_init(CAN_FD_INIT_STRUCT, &fd);
    fd.iso_can_fd_enable = (uint32_t)ENABLE;
    fd.bitrate_switch_enable = (GW_CANFD_BRS_ENABLE != 0U) ?
                               (uint32_t)ENABLE : (uint32_t)DISABLE;
    /* The public frame API supports the full ISO CAN-FD payload range. Configure
     * mailbox RAM for 64-byte frames so a legal long frame can never spill into
     * adjacent mailbox storage or be transmitted with a truncated payload. */
    fd.mailbox_data_size = CAN_MAILBOX_DATA_SIZE_64_BYTES;
#if (GW_CANFD_TDC_ACTIVE != 0U)
    fd.tdc_enable = (uint32_t)ENABLE;
#else
    fd.tdc_enable = (uint32_t)DISABLE;
#endif
    fd.tdc_offset = GW_CANFD_TDC_OFFSET;

    /* In the stable BRS-OFF baseline these fields deliberately equal the
     * nominal 500 kbit/s timing.  They are retained because the FD controller
     * requires a complete data-timing configuration even when no switch occurs. */
    fd.prescaler = GW_CANFD_DATA_PRESCALER;
    fd.resync_jump_width = GW_CANFD_DATA_SJW;
    fd.prop_time_segment = GW_CANFD_DATA_PROP_SEG;
    fd.time_segment_1 = GW_CANFD_DATA_SEG1;
    fd.time_segment_2 = GW_CANFD_DATA_SEG2;
    can_fd_config(CAN_PERIPH, &fd);

    /* RX may be armed while inactive, but avoid configuring MB0 as a TX
     * mailbox until after normal mode is entered. GD32H7 erratum 2.15.1 notes
     * that a TX mailbox configured while inactive can miss its first trigger. */
    rx_mailbox_arm(CAN_RX_MB);
    can_auto_busoff_recovery_enable(CAN_PERIPH);

    nvic_irq_enable(GW_CANFD_MESSAGE_IRQn, GW_CANFD_IRQ_PREEMPT_PRIORITY,
                    GW_RTOS_IRQ_SUB_PRIORITY);
#if (GW_CANFD_ERROR_IRQ_ENABLE != 0U)
    nvic_irq_enable(GW_CANFD_BUSOFF_IRQn, GW_RTOS_IRQ_PREEMPT_PRIORITY,
                    GW_RTOS_IRQ_SUB_PRIORITY);
    nvic_irq_enable(GW_CANFD_ERROR_IRQn, GW_RTOS_IRQ_PREEMPT_PRIORITY,
                    GW_RTOS_IRQ_SUB_PRIORITY);
    nvic_irq_enable(GW_CANFD_FAST_ERROR_IRQn, GW_RTOS_IRQ_PREEMPT_PRIORITY,
                    GW_RTOS_IRQ_SUB_PRIORITY);
#else
    /* Make the bring-up contract explicit. If a warning/error vector ever
     * becomes pending unexpectedly it must not fall into the startup file's
     * weak forever-loop handler. */
    nvic_irq_disable(GW_CANFD_BUSOFF_IRQn);
    nvic_irq_disable(GW_CANFD_ERROR_IRQn);
    nvic_irq_disable(GW_CANFD_FAST_ERROR_IRQn);
    nvic_irq_disable(CAN2_TEC_IRQn);
    nvic_irq_disable(CAN2_REC_IRQn);
    nvic_irq_disable(CAN2_WKUP_IRQn);
#endif

    if (SUCCESS != can_operation_mode_enter(CAN_PERIPH, CAN_NORMAL_MODE)) {
        return false;
    }

    can_mailbox_descriptor_struct txmd;
    can_struct_para_init(CAN_MDSC_STRUCT, &txmd);
    txmd.code = CAN_MB_TX_STATUS_INACTIVE;
    can_mailbox_config(CAN_PERIPH, CAN_TX_MB, &txmd);
    can_interrupt_flag_clear(CAN_PERIPH, flag_for_mailbox(CAN_TX_MB));

    (void)can_interrupt_enable(CAN_PERIPH,
        irq_for_mailbox(CAN_TX_MB));
    (void)can_interrupt_enable(CAN_PERIPH,
        irq_for_mailbox(CAN_RX_MB));
#if (GW_CANFD_ERROR_IRQ_ENABLE != 0U)
    (void)can_interrupt_enable(CAN_PERIPH, CAN_INT_BUSOFF);
    (void)can_interrupt_enable(CAN_PERIPH, CAN_INT_BUSOFF_RECOVERY);
    (void)can_interrupt_enable(CAN_PERIPH, CAN_INT_ERR_SUMMARY);
    (void)can_interrupt_enable(CAN_PERIPH, CAN_INT_ERR_SUMMARY_FD);
#endif
    return true;
}

bool drv_canfd_submit(const canfd_frame_t *frame, uint32_t timeout_ms)
{
    if ((frame == NULL) || !valid_payload_length(frame) ||
        (frame->len > CANFD_MAX_DATA_BYTES) ||
        (frame->extended ? (frame->id > 0x1FFFFFFFU) : (frame->id > 0x7FFU)) ||
        (q_can_tx == NULL) || s_tx_hold) {
        return false;
    }

    TickType_t wait = pdMS_TO_TICKS(timeout_ms);
    if (xQueueSend(q_can_tx, frame, wait) != pdTRUE) {
        ++s_stats.tx_queue_drop;
        return false;
    }

    /* A safe-hold can be asserted while xQueueSend() is blocked. Recheck after
     * enqueue so no frame remains stranded in q_can_tx after the hold reset. */
    if (s_tx_hold) {
        (void)xQueueReset(q_can_tx);
        ++s_stats.tx_queue_drop;
        return false;
    }
    ++s_stats.tx_queued;

    if (can_task_handle != NULL) {
        (void)xTaskNotifyGive(can_task_handle);
    }
    return true;
}

void drv_canfd_service(void)
{
    /* TX mailbox completion is deferred to task context. RX is intentionally
     * NOT masked or read here: GD32H7 erratum 2.15.6 requires receive mailbox
     * data to be drained promptly in the mailbox interrupt path. */
    (void)can_interrupt_disable(CAN_PERIPH, irq_for_mailbox(CAN_TX_MB));

    /* Consume the mailbox completion before policy code is allowed to change
     * s_tx_active. This preserves the terminal TX event and its error counters. */
    tx_completion_service();
    handle_status_flags();
    tx_start_next();

    /* Safe-hold is intentionally latched until reset for high-speed BRS test
     * builds. Stable BRS-OFF builds normally leave s_tx_hold false. */
    if (!s_tx_hold) {
        (void)can_interrupt_enable(CAN_PERIPH, irq_for_mailbox(CAN_TX_MB));
    }
}

void drv_canfd_isr_message(void)
{
    BaseType_t hpw = pdFALSE;
    bool wake_task = false;
    ++s_stats.message_irq_count;

    /* Drain RX first, directly in the mailbox ISR.  This is intentionally more
     * work than the old deferred design: GD32H7 erratum 2.15.6 documents data
     * corruption when the current mailbox is not read before the next frame is
     * moved in.  Priority 2 is the highest FreeRTOS-safe interrupt priority in
     * this project, allowing xQueueSendFromISR while minimizing RX latency. */
    if (SET == can_interrupt_flag_get(CAN_PERIPH, flag_for_mailbox(CAN_RX_MB))) {
        canfd_frame_t frame;
        ++s_stats.rx_irq_count;
        if (rx_mailbox_read_frame(CAN_RX_MB, &frame)) {
            if ((q_can_rx == NULL) ||
                (xQueueSendFromISR(q_can_rx, &frame, &hpw) != pdTRUE)) {
                ++s_stats.rx_queue_drop;
            } else {
                wake_task = true;
            }
        }
    }

    /* TX completion remains task-owned. Mask the level/pending source until the
     * task acknowledges the descriptor, preventing the MB0 IRQ-storm regression. */
    if (SET == can_interrupt_flag_get(CAN_PERIPH, flag_for_mailbox(CAN_TX_MB))) {
        (void)can_interrupt_disable(CAN_PERIPH, irq_for_mailbox(CAN_TX_MB));
        ++s_stats.tx_irq_count;
        wake_task = true;
    }

    if (wake_task && (can_task_handle != NULL)) {
        vTaskNotifyGiveFromISR(can_task_handle, &hpw);
    }
    portYIELD_FROM_ISR(hpw);
}

void drv_canfd_isr_status(void)
{
    BaseType_t hpw = pdFALSE;
    if (can_task_handle != NULL) {
        vTaskNotifyGiveFromISR(can_task_handle, &hpw);
    }
    portYIELD_FROM_ISR(hpw);
}

void drv_canfd_isr_unexpected_tec(void)
{
    ++s_stats.unexpected_tec_irq_count;
    nvic_irq_disable(CAN2_TEC_IRQn);
}

void drv_canfd_isr_unexpected_rec(void)
{
    ++s_stats.unexpected_rec_irq_count;
    nvic_irq_disable(CAN2_REC_IRQn);
}

void drv_canfd_isr_unexpected_wkup(void)
{
    ++s_stats.unexpected_wkup_irq_count;
    nvic_irq_disable(CAN2_WKUP_IRQn);
}

bool drv_canfd_tx_is_held(void)
{
    return s_tx_hold;
}

void drv_canfd_get_stats(canfd_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}
