#ifndef DRV_CANFD_H
#define DRV_CANFD_H

#include <stdbool.h>
#include <stdint.h>
#include "gw_error.h"

#define CANFD_MAX_DATA_BYTES 64U

typedef struct {
    uint32_t id;
    uint8_t data[CANFD_MAX_DATA_BYTES];
    uint8_t len;
    bool extended;
    bool fd;
    bool brs;
    bool esi;
    uint16_t timestamp;
} canfd_frame_t;

typedef struct {
    uint32_t message_irq_count;
    uint32_t rx_irq_count;
    uint32_t tx_irq_count;
    uint32_t rx_frames;
    uint32_t rx_bytes;
    uint32_t rx_fd_frames;
    uint32_t rx_brs_frames;
    uint32_t rx_overrun;
    uint32_t rx_read_error;
    uint32_t rx_queue_drop;
    uint32_t tx_queued;
    uint32_t tx_started;
    uint32_t tx_completed;      /* mailbox reached a terminal state */
    uint32_t tx_success;
    uint32_t tx_bytes;        /* terminal state without new TEC/fdTEC growth */
    uint32_t tx_failed;         /* terminal state accompanied by TX error growth */
    uint32_t tx_aborted;
    uint32_t tx_hold_count;
    uint32_t tx_spurious_irq_count; /* MB0 pending while no SW TX was active */
    uint32_t tx_queue_drop;
    uint32_t busoff_count;
    uint32_t busoff_recovery_count;
    uint32_t error_irq_count;
    uint32_t fd_error_irq_count;
    uint32_t tdc_value;
    uint32_t tdc_out_of_range_count;
    uint32_t error_state;
    uint32_t rx_error_count;
    uint32_t tx_error_count;
    uint32_t fd_rx_error_count;
    uint32_t fd_tx_error_count;
    uint32_t ack_error_count;
    uint32_t stuff_error_count;
    uint32_t form_error_count;
    uint32_t crc_error_count;
    uint32_t bit_error_count;
    uint32_t fd_stuff_error_count;
    uint32_t fd_form_error_count;
    uint32_t fd_crc_error_count;
    uint32_t fd_bit_error_count;
    uint32_t unexpected_tec_irq_count;
    uint32_t unexpected_rec_irq_count;
    uint32_t unexpected_wkup_irq_count;
} canfd_stats_t;

bool drv_canfd_init(void);
bool drv_canfd_submit(const canfd_frame_t *frame, uint32_t timeout_ms);
void drv_canfd_service(void);
void drv_canfd_isr_message(void);
void drv_canfd_isr_status(void);
void drv_canfd_isr_unexpected_tec(void);
void drv_canfd_isr_unexpected_rec(void);
void drv_canfd_isr_unexpected_wkup(void);
bool drv_canfd_tx_is_held(void);
void drv_canfd_get_stats(canfd_stats_t *out);

#endif
