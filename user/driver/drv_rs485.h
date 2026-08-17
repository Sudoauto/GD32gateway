/*!
    \file    drv_rs485.h
    \brief   UART4 RS485 DMA driver

    Hardware:
    - UART4, TX=PB6(AF14), RX=PB12(AF14), DE//RE=PB4
    - DMA0 CH0 = UART4 TX (request 80)
    - DMA0 CH1 = UART4 RX (request 79)
*/

#ifndef DRV_RS485_H
#define DRV_RS485_H

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "bsp_board.h"

#define RS485_NTF_RX_EVENT      (1UL << 0)
#define RS485_NTF_TX_DONE       (1UL << 1)
#define RS485_NTF_UART_ERROR    (1UL << 2)

#define RS485_RX_DMA_BUF_SIZE   GW_RS485_RX_DMA_BUFFER_SIZE
#define RS485_TX_DMA_BUF_SIZE   GW_RS485_TX_DMA_BUFFER_SIZE

typedef enum {
    RS485_PARITY_NONE = 0,
    RS485_PARITY_EVEN,
    RS485_PARITY_ODD
} rs485_parity_t;

typedef struct {
    uint32_t baudrate;
    uint8_t  data_bits;
    uint8_t  stop_bits;
    rs485_parity_t parity;
} rs485_config_t;

typedef struct {
    uint32_t tx_dma_start_count;
    uint32_t tx_bytes;
    uint32_t tx_dma_ftf_count;
    uint32_t tx_tc_complete_count;
    uint32_t rx_idle_count;
    uint32_t rx_bytes;
    uint32_t rx_dma_ftf_count;
    uint32_t rx_dma_spurious_ftf_count;
    uint32_t dma_error_count;
    uint32_t uart_error_count;
} rs485_dma_stats_t;

bool drv_rs485_init(const rs485_config_t *cfg);
bool drv_rs485_dma_init(void);

void drv_rs485_set_tx(void);
void drv_rs485_set_rx(void);

bool drv_rs485_tx_dma_start(const uint8_t *data, uint16_t len);
bool drv_rs485_tx_busy(void);
void drv_rs485_abort_tx(void);

/* RX is single-shot. max_len limits the DMA transfer to the largest frame the
 * current transaction may legitimately return (0 selects the full buffer).
 * An IDLE/full/error ISR freezes DMA before notifying the owner task. */
void drv_rs485_rx_arm(uint16_t max_len);
bool drv_rs485_rx_resume(void);
void drv_rs485_rx_abort(void);
uint16_t drv_rs485_rx_read(uint8_t *dst, uint16_t max_len);
uint16_t drv_rs485_rx_snapshot_len(void);


void drv_rs485_isr_uart(void);
void drv_rs485_isr_tx_dma(void);
void drv_rs485_isr_rx_dma(void);
void drv_rs485_set_task_handle(TaskHandle_t handle);
void drv_rs485_get_stats(rs485_dma_stats_t *out);

#endif /* DRV_RS485_H */
