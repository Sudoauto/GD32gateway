#include "drv_rs485.h"
#include "drv_canfd.h"
#include "bsp_debug_uart.h"
#include "gw_ethernetif.h"
#include <stdint.h>

void UART4_IRQHandler(void);
void DMA0_Channel0_IRQHandler(void);
void DMA0_Channel1_IRQHandler(void);
void CAN2_Message_IRQHandler(void);
void CAN2_Busoff_IRQHandler(void);
void CAN2_Error_IRQHandler(void);
void CAN2_FastError_IRQHandler(void);
void CAN2_TEC_IRQHandler(void);
void CAN2_REC_IRQHandler(void);
void CAN2_WKUP_IRQHandler(void);
void ENET0_IRQHandler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);

void UART4_IRQHandler(void)
{
    drv_rs485_isr_uart();
}

void DMA0_Channel0_IRQHandler(void)
{
    drv_rs485_isr_tx_dma();
}

void DMA0_Channel1_IRQHandler(void)
{
    drv_rs485_isr_rx_dma();
}

/* CAN ISR contract:
 * - RX mailbox data is copied immediately into q_can_rx at the highest
 *   FreeRTOS-safe priority, minimizing the GD32H7 mailbox overwrite window.
 * - TX completion is only masked/notified here and finalized by can_task.
 * - Protocol decode, Device Manager and Point DB remain task-context only. */
void CAN2_Message_IRQHandler(void)
{
    drv_canfd_isr_message();
}

void CAN2_Busoff_IRQHandler(void)
{
    drv_canfd_isr_status();
}

void CAN2_Error_IRQHandler(void)
{
    drv_canfd_isr_status();
}

void CAN2_FastError_IRQHandler(void)
{
    drv_canfd_isr_status();
}

/* Defensive handlers for vectors that are intentionally disabled during M3
 * bring-up. A spurious/premature enable must not fall into the startup weak
 * handler and look like a whole-gateway freeze. */
void CAN2_TEC_IRQHandler(void)
{
    drv_canfd_isr_unexpected_tec();
}

void CAN2_REC_IRQHandler(void)
{
    drv_canfd_isr_unexpected_rec();
}

void CAN2_WKUP_IRQHandler(void)
{
    drv_canfd_isr_unexpected_wkup();
}


void ENET0_IRQHandler(void)
{
    gw_ethernetif_isr();
}

static void fatal_write_literal(const char *s)
{
    size_t n = 0U;
    while (s[n] != '\0') ++n;
    bsp_debug_uart_write(s, n);
}

static void fatal_write_hex32(uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    char out[10];
    out[0] = '0';
    out[1] = 'x';
    for (uint32_t i = 0U; i < 8U; ++i) {
        out[2U + i] = hex[(value >> (28U - (i * 4U))) & 0xFU];
    }
    bsp_debug_uart_write(out, sizeof(out));
}

static void fatal_field(const char *name, uint32_t value)
{
    fatal_write_literal(name);
    fatal_write_hex32(value);
    fatal_write_literal("\r\n");
}

__attribute__((noreturn)) static void fault_dump_and_halt(const char *kind,
                                                           const uint32_t *stack)
{
    __disable_irq();
    fatal_write_literal("\r\n[FATAL] ");
    fatal_write_literal(kind);
    fatal_write_literal("\r\n");

    if (stack != NULL) {
        fatal_field(" R0   =", stack[0]);
        fatal_field(" R1   =", stack[1]);
        fatal_field(" R2   =", stack[2]);
        fatal_field(" R3   =", stack[3]);
        fatal_field(" R12  =", stack[4]);
        fatal_field(" LR   =", stack[5]);
        fatal_field(" PC   =", stack[6]);
        fatal_field(" xPSR =", stack[7]);
    }
    fatal_field(" CFSR =", SCB->CFSR);
    fatal_field(" HFSR =", SCB->HFSR);
    fatal_field(" DFSR =", SCB->DFSR);
    fatal_field(" AFSR =", SCB->AFSR);
    fatal_field(" MMFAR=", SCB->MMFAR);
    fatal_field(" BFAR =", SCB->BFAR);
    fatal_field(" SHCSR=", SCB->SHCSR);
    fatal_field(" MSP  =", __get_MSP());
    fatal_field(" PSP  =", __get_PSP());
    for (;;) { __NOP(); }
}

#if defined(__arm__) || defined(__thumb__) || defined(__ARM_ARCH)
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b hardfault_entry_c\n");
}

__attribute__((used, noreturn)) void hardfault_entry_c(uint32_t *stack)
{
    fault_dump_and_halt("HardFault", stack);
}
#else
/* Host syntax/static-analysis fallback. The embedded ARM build uses the naked
 * handler above and therefore retains the stacked PC/LR. */
void HardFault_Handler(void)
{
    fault_dump_and_halt("HardFault", NULL);
}
#endif

/* The configurable fault handlers retain the same direct-UART diagnostics.
 * Their C entry is sufficient because the primary field issue currently
 * escalates to HardFault; CFSR/MMFAR/BFAR still identify the exact class. */
void MemManage_Handler(void)
{
    fault_dump_and_halt("MemManage", NULL);
}

void BusFault_Handler(void)
{
    fault_dump_and_halt("BusFault", NULL);
}

void UsageFault_Handler(void)
{
    fault_dump_and_halt("UsageFault", NULL);
}

