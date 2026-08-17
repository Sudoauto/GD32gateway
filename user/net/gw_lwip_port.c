#include "gw_lwip_port.h"
#include "gw_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "gd32h7xx.h"

/* lwIP needs inexpensive randomness for DNS transaction IDs, DHCP XIDs and
 * ephemeral local ports. This is deliberately not advertised as cryptographic
 * randomness. The later TLS layer must use the MCU TRNG. */
uint32_t gw_lwip_rand(void)
{
    static uint32_t state = 0xA341316CU;

    taskENTER_CRITICAL();
    uint32_t mix = (uint32_t)xTaskGetTickCount();
    mix ^= SysTick->VAL;
    mix ^= SCB->CPUID;
    state ^= mix + 0x9E3779B9U;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    if (state == 0U) {
        state = 0x6D2B79F5U;
    }
    uint32_t result = state;
    taskEXIT_CRITICAL();
    return result;
}

void gw_lwip_platform_assert(const char *message, const char *file, int line)
{
    GW_LOGE("LWIP", "ASSERT %s (%s:%d)",
            (message != NULL) ? message : "?",
            (file != NULL) ? file : "?", line);
    taskDISABLE_INTERRUPTS();
    for (;;) {
        __NOP();
    }
}
