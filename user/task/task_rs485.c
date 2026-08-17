/*!
    \file    task_rs485.c
    \brief   Single-owner RS485 transaction state machine
*/

#include "task_rs485.h"
#include <string.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "task.h"
#include "bsp_debug_uart.h"
#include "drv_rs485.h"
#include "gateway_build_config.h"
#include "gw_message.h"
#include "gw_config.h"
#include "gw_log.h"
#include "gw_uplink.h"
#include "gw_watchdog.h"
#include "modbus_rtu_master.h"
#include "rs485_bus_manager.h"
#include "rtos_objects.h"

#define RS485_TASK_STACK_WORDS  1024U
#define RS485_TASK_PRIORITY     5U
#define RS485_RX_TMP_SIZE       RS485_RX_DMA_BUF_SIZE
#define RESULT_QUEUE_WAIT_MS    20U
#define RS485_TX_GUARD_MS        20U

typedef enum {
    RS485_STATE_IDLE = 0,
    RS485_STATE_TX_WAIT,
    RS485_STATE_RX_WAIT,
} rs485_state_t;

static StaticTask_t s_task_cb;
static StackType_t s_stack[RS485_TASK_STACK_WORDS];
static rs485_state_t s_state;
static rs485_transaction_t s_active;
static bool s_have_active;
static TickType_t s_deadline;
static uint8_t s_retries_left;
static uint8_t s_rx_buf[RS485_RX_TMP_SIZE];
static TickType_t s_next_tx_tick;
static rs485_task_stats_t s_stats;
static rs485_config_t s_line_cfg;

static void publish_active_frame(const uint8_t *data, uint16_t len,
                                 bool tx_direction, gw_err_t result)
{
    /* A terminal timeout/I/O failure has no RX bytes, but it is still a
     * communication event that the upper host needs for diagnosis. */
    if ((data == NULL) || (len == 0U)) {
        if (!tx_direction && (result != GW_OK)) {
            if (s_active.protocol == RS485_PROTO_MODBUS_RTU) {
                uint8_t slave = 0U;
                if ((s_active.request != NULL) && (s_active.request->length != 0U)) {
                    slave = s_active.request->data[0];
                }
                gw_uplink_publish_modbus(NULL, 0U, s_active.device_id, slave,
                                         false, result);
            } else if (s_active.protocol == RS485_PROTO_RAW) {
                gw_uplink_event_t event;
                memset(&event, 0, sizeof(event));
                event.interface_id = GW_IF_RS485_0;
                event.protocol = GW_PROTO_RS485_RAW;
                event.tx_direction = false;
                event.device_id = s_active.device_id;
                event.result = result;
                gw_uplink_publish_event(&event);
            }
        }
        return;
    }

    if (s_active.protocol == RS485_PROTO_MODBUS_RTU) {
        gw_uplink_publish_modbus(data, len, s_active.device_id, data[0],
                                 tx_direction, result);
        return;
    }

    if (s_active.protocol == RS485_PROTO_RAW) {
        gw_uplink_event_t event;
        memset(&event, 0, sizeof(event));
        event.interface_id = GW_IF_RS485_0;
        event.protocol = GW_PROTO_RS485_RAW;
        event.tx_direction = tx_direction;
        event.device_id = s_active.device_id;
        event.result = result;
        event.length = (len > GW_UPLINK_RAW_MAX) ? GW_UPLINK_RAW_MAX : len;
        memcpy(event.data, data, event.length);
        gw_uplink_publish_event(&event);
    }
}


static uint8_t uart_bits_per_character(void)
{
    uint8_t bits = (uint8_t)(1U + s_line_cfg.data_bits + s_line_cfg.stop_bits);
    if (s_line_cfg.parity != RS485_PARITY_NONE) {
        ++bits;
    }
    return bits;
}

static TickType_t transaction_tx_timeout_ticks(void)
{
    uint64_t bits = (uint64_t)s_active.request->length *
                    (uint64_t)uart_bits_per_character();
    uint32_t tx_ms = (uint32_t)((bits * 1000ULL +
                                 (uint64_t)s_line_cfg.baudrate - 1ULL) /
                                (uint64_t)s_line_cfg.baudrate);
    TickType_t ticks = pdMS_TO_TICKS(tx_ms + RS485_TX_GUARD_MS);
    return (ticks == 0U) ? 1U : ticks;
}

static TickType_t modbus_t35_ticks(void)
{
    uint32_t silence_ms;

    if (s_line_cfg.baudrate > 19200U) {
        /* Modbus Serial Line recommends fixed t3.5=1.75 ms above 19.2 kbaud.
         * Round up to 2 ms because the scheduler tick is millisecond based. */
        silence_ms = 2U;
    } else {
        uint64_t us = (3500000ULL * (uint64_t)uart_bits_per_character() +
                       (uint64_t)s_line_cfg.baudrate - 1ULL) /
                      (uint64_t)s_line_cfg.baudrate;
        silence_ms = (uint32_t)((us + 999ULL) / 1000ULL);
        if (silence_ms == 0U) {
            silence_ms = 1U;
        }
    }

    TickType_t ticks = pdMS_TO_TICKS(silence_ms);
    return (ticks == 0U) ? 1U : ticks;
}

static void mark_bus_silence(void)
{
    s_next_tx_tick = xTaskGetTickCount() + modbus_t35_ticks();
}

static void wait_bus_silence(void)
{
    TickType_t now = xTaskGetTickCount();
    int32_t remaining = (int32_t)(s_next_tx_tick - now);
    if (remaining > 0) {
        vTaskDelay((TickType_t)remaining);
    }
}

static void clear_task_notifications(void)
{
    uint32_t ignored = 0U;
    (void)xTaskNotifyWait(0U, UINT32_MAX, &ignored, 0U);
}

static uint16_t transaction_rx_dma_limit(void)
{
    if ((s_active.expected_rx_length > 0U) &&
        (s_active.expected_rx_length <= RS485_RX_DMA_BUF_SIZE)) {
        return s_active.expected_rx_length;
    }

    if ((s_active.protocol == RS485_PROTO_MODBUS_RTU) &&
        (s_active.request != NULL) && (s_active.request->length >= 6U)) {
        uint8_t fc = s_active.request->data[1];
        if ((fc == 0x01U) || (fc == 0x02U)) {
            uint16_t qty = (uint16_t)(
                ((uint16_t)s_active.request->data[4] << 8U) |
                (uint16_t)s_active.request->data[5]);
            if ((qty > 0U) && (qty <= 2000U)) {
                return (uint16_t)(5U + ((qty + 7U) / 8U));
            }
        } else if ((fc == 0x03U) || (fc == 0x04U)) {
            uint16_t qty = (uint16_t)(
                ((uint16_t)s_active.request->data[4] << 8U) |
                (uint16_t)s_active.request->data[5]);
            if ((qty > 0U) && (qty <= 125U)) {
                return (uint16_t)(5U + (qty * 2U));
            }
        }
    }

    return RS485_RX_DMA_BUF_SIZE;
}

#if (GW_RS485_RX_DIAGNOSTIC_LOG != 0)
static uint16_t diagnostic_expected_rx_length(const uint8_t *rx, uint16_t rx_len)
{
    if (s_active.expected_rx_length != 0U) {
        return s_active.expected_rx_length;
    }

    if (s_active.protocol != RS485_PROTO_MODBUS_RTU) {
        return 0U;
    }

    /* Once an exception function code is visible, the RTU response is 5 bytes. */
    if ((rx != NULL) && (rx_len >= 2U) && ((rx[1] & 0x80U) != 0U)) {
        return 5U;
    }

    /* For FC03/FC04 the request already tells us the normal response length.
     * This is diagnostic-only and deliberately does not trust a possibly
     * corrupted response byte-count field. */
    if ((s_active.request != NULL) && (s_active.request->length >= 6U)) {
        uint8_t fc = s_active.request->data[1];
        if ((fc == 0x01U) || (fc == 0x02U)) {
            uint16_t qty = (uint16_t)(
                ((uint16_t)s_active.request->data[4] << 8U) |
                (uint16_t)s_active.request->data[5]);
            if ((qty > 0U) && (qty <= 2000U)) {
                return (uint16_t)(5U + ((qty + 7U) / 8U));
            }
        } else if ((fc == 0x03U) || (fc == 0x04U)) {
            uint16_t qty = (uint16_t)(
                ((uint16_t)s_active.request->data[4] << 8U) |
                (uint16_t)s_active.request->data[5]);
            if ((qty > 0U) && (qty <= 125U)) {
                return (uint16_t)(5U + (qty * 2U));
            }
        }
    }

    return modbus_rtu_expected_response_length(rx, rx_len);
}

static void log_rx_frame(const uint8_t *data, uint16_t len, uint16_t expected)
{
    static const char hex[] = "0123456789ABCDEF";

    GW_LOGD("RS485", "RX(DMA) len=%u expected=%u",
            (unsigned int)len, (unsigned int)expected);

    if ((data == NULL) || (len == 0U)) {
        BSP_DEBUG_UART_WRITE_LITERAL("[D][RS485] RXHEX: <empty>\r\n");
        return;
    }

    /* Print every received byte. Keep each line short so the debug logger's
     * fixed line size cannot truncate a long DMA snapshot. */
    for (uint16_t base = 0U; base < len; base = (uint16_t)(base + 16U)) {
        char line[96];
        size_t pos = 0U;
        static const char prefix[] = "[D][RS485] RXHEX ";
        uint16_t end = (uint16_t)(base + 16U);
        if (end > len) {
            end = len;
        }

        memcpy(line, prefix, sizeof(prefix) - 1U);
        pos = sizeof(prefix) - 1U;

        /* Four hexadecimal digits are enough for the 256-byte DMA buffer. */
        line[pos++] = hex[(base >> 12U) & 0x0FU];
        line[pos++] = hex[(base >> 8U) & 0x0FU];
        line[pos++] = hex[(base >> 4U) & 0x0FU];
        line[pos++] = hex[base & 0x0FU];
        line[pos++] = ':';
        line[pos++] = ' ';

        for (uint16_t i = base; i < end; ++i) {
            line[pos++] = hex[(data[i] >> 4U) & 0x0FU];
            line[pos++] = hex[data[i] & 0x0FU];
            if ((i + 1U) < end) {
                line[pos++] = ' ';
            }
        }

        line[pos++] = '\r';
        line[pos++] = '\n';
        bsp_debug_uart_write(line, pos);
    }
}
#endif

#if (GW_RS485_TX_DIAGNOSTIC_LOG != 0)
static void log_tx_frame(const uint8_t *data, uint16_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    static const char prefix[] = "[D][RS485] TX(DMA): ";
    char line[160];
    size_t pos = 0U;
    uint16_t shown = len;

    if (data == NULL) {
        return;
    }
    if (shown > 32U) {
        shown = 32U;
    }

    memcpy(line, prefix, sizeof(prefix) - 1U);
    pos = sizeof(prefix) - 1U;

    for (uint16_t i = 0U; i < shown; ++i) {
        if ((pos + 3U) >= sizeof(line)) {
            break;
        }
        line[pos++] = hex[(data[i] >> 4U) & 0x0FU];
        line[pos++] = hex[data[i] & 0x0FU];
        if ((i + 1U) < shown) {
            line[pos++] = ' ';
        }
    }

    if ((shown < len) && ((pos + 4U) < sizeof(line))) {
        line[pos++] = ' ';
        line[pos++] = '.';
        line[pos++] = '.';
        line[pos++] = '.';
    }
    if ((pos + 2U) <= sizeof(line)) {
        line[pos++] = '\r';
        line[pos++] = '\n';
    }
    bsp_debug_uart_write(line, pos);
}
#endif

static void release_transaction(void)
{
    if (s_have_active && (s_active.request != NULL)) {
        gw_msg_free(s_active.request);
        s_active.request = NULL;
    }
    s_have_active = false;
}

static void publish_result(gw_err_t result, uint8_t exception_code,
                           const uint8_t *rx, uint16_t rx_len)
{
    modbus_result_t r;
    memset(&r, 0, sizeof(r));
    r.transaction_id = s_active.transaction_id;
    r.device_id = s_active.device_id;
    r.result = result;
    r.slave_address = ((s_active.request != NULL) &&
                       (s_active.request->length != 0U))
                      ? s_active.request->data[0] : 0U;
    r.exception_code = exception_code;
    r.context = s_active.context;

    /* Preserve the wire frame even when validation reports CRC/protocol error.
     * The result code still tells decoders not to treat it as valid data, while
     * diagnostics/uplink can expose exactly what arrived on the bus. */
    if ((rx != NULL) && (rx_len > 0U)) {
        if (rx_len > GW_MSG_BLOCK_SIZE) {
            r.result = GW_ERR_FULL;
        } else {
            gw_msg_block_t *payload = gw_msg_alloc(0U);
            if (payload == NULL) {
                r.result = GW_ERR_NO_MEMORY;
            } else {
                memcpy(payload->data, rx, rx_len);
                payload->length = rx_len;
                r.payload = payload;
                r.payload_length = rx_len;
                if (rx_len >= 2U) {
                    r.function_code = rx[1];
                }
            }
        }
    }

    if (xQueueSend(q_modbus_result, &r,
                   pdMS_TO_TICKS(RESULT_QUEUE_WAIT_MS)) != pdTRUE) {
        if (r.payload != NULL) {
            gw_msg_free(r.payload);
        }
        if (g_system_events != NULL) {
            (void)xEventGroupSetBits(g_system_events, EVT_SYSTEM_DEGRADED);
        }
        BSP_DEBUG_UART_WRITE_LITERAL("[RS485] result queue full\r\n");
    }
}

static void finish_transaction(gw_err_t result, uint8_t exception_code,
                               const uint8_t *rx, uint16_t rx_len)
{
    switch (result) {
    case GW_OK: ++s_stats.final_ok_count; break;
    case GW_ERR_TIMEOUT: ++s_stats.final_timeout_count; break;
    case GW_ERR_CRC: ++s_stats.final_crc_count; break;
    case GW_ERR_PROTOCOL: ++s_stats.final_protocol_count; break;
    case GW_ERR_IO: ++s_stats.final_io_count; break;
    default: break;
    }

    /* Publish the actual bus frame before the transaction storage is released.
     * This records successful replies, Modbus exceptions and terminal CRC/
     * protocol failures in the same northbound event envelope. */
    publish_active_frame(rx, rx_len, false, result);

    drv_rs485_abort_tx();
    drv_rs485_rx_abort();
    mark_bus_silence();
    publish_result(result, exception_code, rx, rx_len);
    release_transaction();
    s_state = RS485_STATE_IDLE;
    rs485_bus_set_idle_internal(true);
}

static bool start_attempt(void)
{
    /* Quiesce the previous attempt immediately. This is especially important
     * for retry paths entered from a UART/DMA error while TX could still own
     * the bus. */
    drv_rs485_abort_tx();
    drv_rs485_rx_abort();
    clear_task_notifications();

    /* Every new request/retry observes the Modbus RTU inter-frame silent time.
     * The dedicated bus-owner task may sleep here; no CPU polling is involved. */
    wait_bus_silence();

    /* Disable the receiver at the transceiver before arming RX DMA so idle-bus
     * noise cannot populate the fresh frame buffer. RX DMA is ready before TX
     * starts, which prevents a fast slave response from being missed at TC. */
    drv_rs485_set_tx();
    drv_rs485_rx_arm(transaction_rx_dma_limit());

#if (GW_RS485_TX_DIAGNOSTIC_LOG != 0)
    log_tx_frame(s_active.request->data, s_active.request->length);
#endif
    publish_active_frame(s_active.request->data, s_active.request->length,
                         true, GW_OK);
    ++s_stats.attempt_count;
    if (!drv_rs485_tx_dma_start(s_active.request->data,
                                s_active.request->length)) {
        drv_rs485_rx_abort();
        drv_rs485_set_rx();
        return false;
    }

    s_state = RS485_STATE_TX_WAIT;
    s_deadline = xTaskGetTickCount() + transaction_tx_timeout_ticks();
    return true;
}

static bool retry_or_finish(gw_err_t error, uint8_t exception_code,
                            const uint8_t *rx, uint16_t rx_len)
{
    /* A Modbus exception is a valid response from the slave; retrying it does
     * not improve link reliability and can repeat a rejected operation. */
    if ((error == GW_ERR_PROTOCOL) && (exception_code != 0U)) {
        finish_transaction(error, exception_code, rx, rx_len);
        return false;
    }

    if (s_retries_left > 0U) {
        /* Keep failed attempts visible even when the transaction is retried. */
        publish_active_frame(rx, rx_len, false, error);
        --s_retries_left;
        ++s_stats.retry_count;
        GW_LOGW("RS485", "txn=%lu retry err=%ld left=%u",
                (unsigned long)s_active.transaction_id, (long)error,
                (unsigned)s_retries_left);
        mark_bus_silence();
        if (start_attempt()) {
            return true;
        }
        error = GW_ERR_IO;
    }

    finish_transaction(error, exception_code, rx, rx_len);
    return false;
}

static void rs485_task(void *argument)
{
    (void)argument;

    memset(&s_stats, 0, sizeof(s_stats));

    gw_runtime_config_t runtime;
    gw_config_get_runtime(&runtime);
    s_line_cfg.baudrate = runtime.rs485_baudrate;
    s_line_cfg.data_bits = runtime.rs485_data_bits;
    s_line_cfg.stop_bits = runtime.rs485_stop_bits;
    s_line_cfg.parity = (rs485_parity_t)runtime.rs485_parity;

    if (!drv_rs485_init(&s_line_cfg) || !drv_rs485_dma_init()) {
        if (g_system_events != NULL) {
            (void)xEventGroupSetBits(g_system_events, EVT_SYSTEM_DEGRADED);
        }
        BSP_DEBUG_UART_WRITE_LITERAL("[RS485] init failed\r\n");
        vTaskSuspend(NULL);
    }

    drv_rs485_set_task_handle(xTaskGetCurrentTaskHandle());
    drv_rs485_abort_tx();
    drv_rs485_rx_abort();

    s_state = RS485_STATE_IDLE;
    s_have_active = false;
    s_next_tx_tick = xTaskGetTickCount();
    rs485_bus_set_idle_internal(true);

    if (g_system_events != NULL) {
        (void)xEventGroupSetBits(g_system_events, EVT_RS485_READY);
    }
    GW_LOGI("RS485", "UART4 ready, %lu baud", (unsigned long)s_line_cfg.baudrate);

    for (;;) {
        gw_watchdog_beat(GW_WD_RS485);
        if (s_state == RS485_STATE_IDLE) {
            /* Runtime baud/parity changes are applied only while the bus owner
             * is idle, so no in-flight RTU frame is reconfigured mid-byte. */
            gw_runtime_config_t rc;
            gw_config_get_runtime(&rc);
            if ((rc.rs485_baudrate != s_line_cfg.baudrate) ||
                (rc.rs485_data_bits != s_line_cfg.data_bits) ||
                (rc.rs485_stop_bits != s_line_cfg.stop_bits) ||
                (rc.rs485_parity != (uint8_t)s_line_cfg.parity)) {
                rs485_config_t next = {rc.rs485_baudrate, rc.rs485_data_bits, rc.rs485_stop_bits, (rs485_parity_t)rc.rs485_parity};
                drv_rs485_abort_tx(); drv_rs485_rx_abort();
                if (drv_rs485_init(&next)) { s_line_cfg = next; GW_LOGI("RS485","runtime line config applied: %lu baud",(unsigned long)next.baudrate); }
                else GW_LOGE("RS485","runtime line config rejected");
            }
            if (xQueueReceive(q_rs485_txn, &s_active, pdMS_TO_TICKS(100U)) != pdTRUE) {
                continue;
            }

            s_have_active = true;
            s_retries_left = s_active.retry;
            rs485_bus_set_idle_internal(false);

            if (!start_attempt()) {
                finish_transaction(GW_ERR_IO, 0U, NULL, 0U);
            }
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        TickType_t wait = ((int32_t)(s_deadline - now) > 0)
                          ? (s_deadline - now) : 0U;
        /* Never sleep for the entire application-configured response timeout.
         * Periodic wakeups keep the business watchdog honest even when a field
         * device uses a multi-second Modbus timeout. The absolute deadline
         * below still controls transaction semantics. */
        TickType_t service_slice = pdMS_TO_TICKS(500U);
        if ((wait > service_slice) && (service_slice != 0U)) wait = service_slice;
        uint32_t ntf = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &ntf, wait);

        if (!s_have_active) {
            drv_rs485_abort_tx();
            drv_rs485_rx_abort();
            s_state = RS485_STATE_IDLE;
            rs485_bus_set_idle_internal(true);
            continue;
        }

        if ((ntf & RS485_NTF_UART_ERROR) != 0U) {
            (void)retry_or_finish(GW_ERR_IO, 0U, NULL, 0U);
            continue;
        }

        if ((s_state == RS485_STATE_TX_WAIT) &&
            ((ntf & RS485_NTF_TX_DONE) != 0U)) {
            s_state = RS485_STATE_RX_WAIT;
            /* Response timeout begins only after the final stop bit left. */
            s_deadline = xTaskGetTickCount() +
                         pdMS_TO_TICKS(s_active.timeout_ms);
        }

        if ((ntf & RS485_NTF_RX_EVENT) != 0U) {
            if (s_state != RS485_STATE_RX_WAIT) {
                /* A frame before TX completion is stale/noise. DMA is already
                 * frozen by the ISR, so restart the whole attempt cleanly. */
                (void)retry_or_finish(GW_ERR_PROTOCOL, 0U, NULL, 0U);
                continue;
            }

            uint16_t n = drv_rs485_rx_read(s_rx_buf, sizeof(s_rx_buf));
            if (n == 0U) {
#if (GW_RS485_RX_DIAGNOSTIC_LOG != 0)
                log_rx_frame(NULL, 0U, diagnostic_expected_rx_length(NULL, 0U));
#endif
                (void)retry_or_finish(GW_ERR_PROTOCOL, 0U, NULL, 0U);
                continue;
            }

#if (GW_RS485_RX_DIAGNOSTIC_LOG != 0)
            log_rx_frame(s_rx_buf, n,
                         diagnostic_expected_rx_length(s_rx_buf, n));
#endif

            uint16_t expected = s_active.expected_rx_length;
            if (s_active.protocol == RS485_PROTO_MODBUS_RTU) {
                /* A Modbus exception response is always 5 bytes. Do not keep
                 * waiting for the normal FC03/FC06 response length when the
                 * slave has already returned FC|0x80 + exception + CRC. */
                if ((n >= 2U) && ((s_rx_buf[1] & 0x80U) != 0U)) {
                    expected = 5U;
                } else if (expected == 0U) {
                    expected = modbus_rtu_expected_response_length(s_rx_buf, n);
                }
            }

            /* UART IDLE is about one character time, while Modbus permits
             * inter-character gaps below 1.5 character times. A short IDLE
             * therefore freezes DMA for a race-free snapshot but is not always
             * the end of the RTU frame. Resume from the captured offset when
             * the protocol says more bytes are still required. */
            if (s_active.protocol == RS485_PROTO_MODBUS_RTU) {
                if ((expected == 0U) && (n < 3U)) {
                    if (drv_rs485_rx_resume()) {
                        continue;
                    }
                    (void)retry_or_finish(GW_ERR_PROTOCOL, 0U, NULL, 0U);
                    continue;
                }
            }

            if ((expected > 0U) && (n < expected)) {
                if (drv_rs485_rx_resume()) {
                    continue;
                }
                (void)retry_or_finish(GW_ERR_PROTOCOL, 0U, NULL, 0U);
                continue;
            }

            if ((expected == 0U) || (n > expected)) {
                (void)retry_or_finish(GW_ERR_PROTOCOL, 0U, s_rx_buf, n);
                continue;
            }

            if (s_active.protocol == RS485_PROTO_RAW) {
                finish_transaction(GW_OK, 0U, s_rx_buf, n);
                continue;
            }

            if ((s_active.request == NULL) || (s_active.request->length < 2U)) {
                finish_transaction(GW_ERR_STATE, 0U, NULL, 0U);
                continue;
            }

            uint8_t exception_code = 0U;
            gw_err_t valid = modbus_rtu_validate_response_for_request(
                s_rx_buf, n,
                s_active.request->data, s_active.request->length,
                &exception_code);

#if (GW_RS485_RX_DIAGNOSTIC_LOG != 0)
            GW_LOGD("RS485", "RX validate=%ld exception=%u",
                    (long)valid, (unsigned int)exception_code);
#endif

            if (valid == GW_OK) {
                finish_transaction(GW_OK, 0U, s_rx_buf, n);
            } else {
                (void)retry_or_finish(valid, exception_code, s_rx_buf, n);
            }
            continue;
        }

        if ((int32_t)(xTaskGetTickCount() - s_deadline) >= 0) {
            (void)retry_or_finish(GW_ERR_TIMEOUT, 0U, NULL, 0U);
        }
    }
}

void task_rs485_get_stats(rs485_task_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}

void task_rs485_create(void)
{
    rs485_task_handle = xTaskCreateStatic(
        rs485_task, "rs485", RS485_TASK_STACK_WORDS, NULL,
        RS485_TASK_PRIORITY, s_stack, &s_task_cb);
    configASSERT(rs485_task_handle != NULL);
}
