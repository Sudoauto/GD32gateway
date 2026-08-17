/*!
    \file    test_task.c
    \brief   FreeRTOS test task for M0.2B bring-up
*/

#include "test_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bsp_debug_uart.h"
#include <stdio.h>

#define TEST_TASK_STACK_SIZE    256
#define TEST_TASK_PRIORITY      1

static StaticTask_t test_task_tcb;
static StackType_t test_task_stack[TEST_TASK_STACK_SIZE];

/*!
    \brief      test task function
    \param[in]  pvParameters: task parameters (unused)
    \param[out] none
    \retval     none
*/
static void test_task_function(void *pvParameters)
{
    (void)pvParameters;
    TickType_t tick;
    char buffer[64];
    int len;

    while(1) {
        tick = xTaskGetTickCount();
        
        /* format message */
        len = snprintf(buffer, sizeof(buffer), 
                      "FreeRTOS alive, tick=%lu\r\n", 
                      (unsigned long)tick);
        
        if(len > 0 && len < (int)sizeof(buffer)) {
            bsp_debug_uart_write(buffer, (size_t)len);
        }

        /* delay 1000ms */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*!
    \brief      create test task
    \param[in]  none
    \param[out] none
    \retval     none
*/
void test_task_create(void)
{
    TaskHandle_t handle;
    
    handle = xTaskCreateStatic(
        test_task_function,
        "test",
        TEST_TASK_STACK_SIZE,
        NULL,
        TEST_TASK_PRIORITY,
        test_task_stack,
        &test_task_tcb
    );

    configASSERT(handle != NULL);
}
