/*!
    \file    freertos_hooks.c
    \brief   FreeRTOS hook functions
*/

#include "FreeRTOS.h"
#include "task.h"
#include "bsp_debug_uart.h"
#include "gw_time.h"

/*!
    \brief      malloc failed hook
    \param[in]  none
    \param[out] none
    \retval     none
*/





void gw_runtime_stats_init(void) { }
uint32_t gw_runtime_stats_counter(void)
{
    /* FreeRTOS calls this from PendSV while switching tasks.  It must therefore
     * be exception-safe and must not use taskENTER_CRITICAL(), mutexes, queues,
     * or any non-FromISR kernel API. */
    return gw_time_runtime_counter32();
}

void vApplicationTickHook(void)
{
    gw_time_tick_from_isr();
}

void vApplicationMallocFailedHook(void)
{
    BSP_DEBUG_UART_WRITE_LITERAL("[FATAL] Malloc failed\r\n");
    taskDISABLE_INTERRUPTS();
    for(;;) {
    }
}

/*!
    \brief      stack overflow hook
    \param[in]  xTask: task handle
    \param[in]  pcTaskName: task name
    \param[out] none
    \retval     none
*/
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    
    BSP_DEBUG_UART_WRITE_LITERAL("[FATAL] Stack overflow: ");
    if(pcTaskName != NULL) {
        /* print task name - assume null terminated and reasonable length */
        for(int i = 0; i < 16 && pcTaskName[i] != '\0'; i++) {
            bsp_debug_uart_putc(pcTaskName[i]);
        }
    }
    BSP_DEBUG_UART_WRITE_LITERAL("\r\n");
    
    taskDISABLE_INTERRUPTS();
    for(;;) {
    }
}

#if (configSUPPORT_STATIC_ALLOCATION == 1)
/* idle task static memory */
static StaticTask_t idle_task_tcb;
static StackType_t idle_task_stack[configMINIMAL_STACK_SIZE];

/*!
    \brief      get idle task memory
    \param[out] ppxIdleTaskTCBBuffer: pointer to idle task TCB
    \param[out] ppxIdleTaskStackBuffer: pointer to idle task stack
    \param[out] pulIdleTaskStackSize: idle task stack size
    \retval     none
*/
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &idle_task_tcb;
    *ppxIdleTaskStackBuffer = idle_task_stack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

#if (configUSE_TIMERS == 1)
/* timer task static memory */
static StaticTask_t timer_task_tcb;
static StackType_t timer_task_stack[configTIMER_TASK_STACK_DEPTH];

/*!
    \brief      get timer task memory
    \param[out] ppxTimerTaskTCBBuffer: pointer to timer task TCB
    \param[out] ppxTimerTaskStackBuffer: pointer to timer task stack
    \param[out] pulTimerTaskStackSize: timer task stack size
    \retval     none
*/
void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    *ppxTimerTaskTCBBuffer = &timer_task_tcb;
    *ppxTimerTaskStackBuffer = timer_task_stack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
#endif /* configUSE_TIMERS */

#endif /* configSUPPORT_STATIC_ALLOCATION */
