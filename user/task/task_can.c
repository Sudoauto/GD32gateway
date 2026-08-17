#include "task_can.h"
#include <stdio.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "task.h"
#include "can_decoder.h"
#include "drv_canfd.h"
#include "gateway_build_config.h"
#include "gw_log.h"
#include "gw_time.h"
#include "gw_uplink.h"
#include "gw_watchdog.h"
#include "rtos_objects.h"

#define CAN_TASK_STACK_WORDS  1280U
#define CAN_TASK_PRIORITY     5U

static StaticTask_t s_task_cb;
static StackType_t s_stack[CAN_TASK_STACK_WORDS];

static void can_task(void *argument)
{
    (void)argument;

    if (!drv_canfd_init()) {
        GW_LOGE("CAN", "CAN2/CAN-FD init failed");
        (void)xEventGroupSetBits(g_system_events, EVT_SYSTEM_DEGRADED);
        vTaskSuspend(NULL);
    }

    (void)xEventGroupSetBits(g_system_events, EVT_CANFD_READY);
    GW_LOGI("CAN", "CAN2 stable: CAN-FD 500k, BRS=OFF, TDC=OFF, CK_CAN=APB2");

    uint64_t next_maintenance = gw_time_ms();
#if (GW_CANFD_RX_TRACE_ENABLE != 0U)
    uint32_t rx_trace_count = 0U;
#endif
    for (;;) {
        gw_watchdog_beat(GW_WD_CAN);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20U));
        drv_canfd_service();

        canfd_frame_t frame;
        while (xQueueReceive(q_can_rx, &frame, 0U) == pdTRUE) {
#if (GW_CANFD_RX_TRACE_ENABLE != 0U)
            ++rx_trace_count;
            bool trace_this = (rx_trace_count <= GW_CANFD_RX_TRACE_FIRST_N);
            if ((!trace_this) && (GW_CANFD_RX_TRACE_EVERY_N != 0U)) {
                trace_this = ((rx_trace_count % GW_CANFD_RX_TRACE_EVERY_N) == 0U);
            }
            if (trace_this) {
                char hex[3U * 16U + 1U];
                size_t pos = 0U;
                uint8_t shown = (frame.len < 16U) ? frame.len : 16U;
                for (uint8_t i = 0U; i < shown; ++i) {
                    int n = snprintf(&hex[pos], sizeof(hex) - pos,
                                     (i + 1U < shown) ? "%02X " : "%02X",
                                     frame.data[i]);
                    if (n <= 0) {
                        break;
                    }
                    pos += (size_t)n;
                    if (pos >= sizeof(hex)) {
                        pos = sizeof(hex) - 1U;
                        break;
                    }
                }
                hex[pos] = '\0';
                GW_LOGI("CANRX", "id=0x%08lX %s FD=%u BRS=%u len=%u data=%s",
                        (unsigned long)frame.id,
                        frame.extended ? "EXT" : "STD",
                        frame.fd ? 1U : 0U, frame.brs ? 1U : 0U,
                        (unsigned)frame.len, hex);
            }
#endif
            gw_uplink_publish_can(&frame, false);
            can_decoder_process(&frame);
        }

        uint64_t now = gw_time_ms();
        if (now >= next_maintenance) {
            can_decoder_maintenance(now);
            next_maintenance = now + 50U;
        }
    }
}

void task_can_create(void)
{
    can_task_handle = xTaskCreateStatic(
        can_task, "can", CAN_TASK_STACK_WORDS, NULL,
        CAN_TASK_PRIORITY, s_stack, &s_task_cb);
    configASSERT(can_task_handle != NULL);
}
