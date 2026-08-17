/*!
    \file    main.c
    \brief   GD32H759 industrial gateway entry point
*/

#include "bsp_cache.h"
#include "bsp_debug_uart.h"
#include "gateway_app.h"
#include "gateway_build_config.h"
#include "gd32h7xx_misc.h"
#include "FreeRTOS.h"
#include "task.h"

int main(void)
{
    /* GD32 SPL nvic_irq_enable() falls back to PRE2/SUB2 when AIRCR is at
     * its reset value. FreeRTOS Cortex-M7 FromISR APIs require all implemented
     * priority bits to be pre-emption bits, so establish the vendor-compatible
     * PRE4/SUB0 grouping before the scheduler or any peripheral IRQ is enabled. */
    nvic_priority_group_set(NVIC_PRIGROUP_PRE4_SUB0);

    /* SystemInit() in the supplied device file leaves D-cache disabled. */
    bsp_cache_enable();

    bsp_debug_uart_init(GW_DEBUG_BAUDRATE);
    BSP_DEBUG_UART_WRITE_LITERAL("GD32H759 industrial gateway v0.9.5-hmi-ipa-fastpath\r\n");
    BSP_DEBUG_UART_WRITE_LITERAL("[I][SYS] NVIC priority group PRE4_SUB0\r\n");
    BSP_DEBUG_UART_WRITE_LITERAL("[I][SYS] RS485 transport: TX DMA + RX DMA/IDLE\r\n");
    BSP_DEBUG_UART_WRITE_LITERAL("[I][SYS] Services: runtime config + SNTP + alarms/rules + watchdog + syslog/SNMP + offline replay + auth\r\n");
#if (GW_ETH_ENABLE != 0U)
    BSP_DEBUG_UART_WRITE_LITERAL("[I][SYS] Ethernet: ENET0 RMII + LAN8720A + lwIP 2.1.2\r\n[I][SYS] TCP: echo :5000 + authenticated GW-JSONL/config uplink :5001\r\n");
#endif
#if (GW_GUI_ENABLE != 0U)
    BSP_DEBUG_UART_WRITE_LITERAL("[I][SYS] HMI: 5-inch 800x480 RGB + LVGL 9.2.2 + Goodix touch\r\n");
#if (GW_ETH_ENABLE != 0U) && (GW_ETH_RMII_REFCLK_EXTERNAL_50M != 0U)
    BSP_DEBUG_UART_WRITE_LITERAL("[I][SYS] HW: Ethernet PHY RMII 50MHz must be externally supplied; PA8 is LCD R6\r\n");
#endif
#endif
#if (GW_RS485_RX_DIAGNOSTIC_LOG != 0)
    BSP_DEBUG_UART_WRITE_LITERAL("[I][SYS] RS485 RX diagnostic logging: ENABLED\r\n");
#endif
#if (GW_M123_BOARD_VALIDATION_ENABLE != 0)
    BSP_DEBUG_UART_WRITE_LITERAL("[I][SYS] M1.3/M1.4/M2 board validation: ENABLED\r\n");
#endif

    /* Creates all static RTOS objects before any task can use them. */
    gateway_app_init();

    BSP_DEBUG_UART_WRITE_LITERAL("Starting FreeRTOS...\r\n");
    vTaskStartScheduler();

    /* vTaskStartScheduler() only returns when scheduler startup failed. */
    BSP_DEBUG_UART_WRITE_LITERAL("[FATAL] scheduler returned\r\n");
    for (;;) {
    }
}
