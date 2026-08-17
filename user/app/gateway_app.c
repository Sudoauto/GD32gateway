#include "gateway_app.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "device_manager.h"
#include "can_decoder.h"
#include "drv_canfd.h"
#include "drv_rs485.h"
#include "gateway_build_config.h"
#include "gw_log.h"
#include "gw_message.h"
#include "gw_command_router.h"
#include "gw_uplink.h"
#include "gw_types.h"
#include "point_db.h"
#include "poll_scheduler.h"
#include "rs485_smoke_test.h"
#include "rtos_objects.h"
#include "task_data.h"
#include "task_can.h"
#include "gw_time.h"
#include "gw_net_manager.h"
#include "gw_tcp_server.h"
#include "task_rs485.h"
#include "gw_gui.h"
#include "gw_ota.h"
#include "gw_watchdog.h"
#include "gw_diagnostics.h"
#include "gw_snmp.h"
#include "gw_syslog.h"
#include "gw_sntp.h"
#include "gw_config.h"

#if (GW_M123_BOARD_VALIDATION_ENABLE != 0)
#define M123_VAL_STACK_WORDS  768U
#define M123_VAL_PRIORITY     2U

static StaticTask_t s_m123_val_task_cb;
static StackType_t s_m123_val_stack[M123_VAL_STACK_WORDS];

static const char *validation_device_state(device_state_t state)
{
    switch (state) {
    case DEVICE_DISABLED: return "DISABLED";
    case DEVICE_INIT: return "INIT";
    case DEVICE_ONLINE: return "ONLINE";
    case DEVICE_OFFLINE: return "OFFLINE";
    case DEVICE_ERROR: return "ERROR";
    default: return "?";
    }
}

static const char *validation_quality(gw_quality_t quality)
{
    switch (quality) {
    case GW_QUALITY_GOOD: return "GOOD";
    case GW_QUALITY_STALE: return "STALE";
    case GW_QUALITY_TIMEOUT: return "TIMEOUT";
    case GW_QUALITY_BAD: return "BAD";
    case GW_QUALITY_OFFLINE: return "OFFLINE";
    case GW_QUALITY_INVALID: return "INVALID";
    default: return "?";
    }
}

static int32_t validation_value_x1000(const gw_point_t *point)
{
    if (point == NULL) {
        return 0;
    }
    switch (point->type) {
    case GW_VALUE_BOOL: return point->value.b ? 1000 : 0;
    case GW_VALUE_U16: return (int32_t)point->value.u16 * 1000;
    case GW_VALUE_I16: return (int32_t)point->value.i16 * 1000;
    case GW_VALUE_U32:
        return (point->value.u32 <= 2147483U) ?
               (int32_t)(point->value.u32 * 1000U) : 2147483647;
    case GW_VALUE_I32:
        if (point->value.i32 > 2147483) return 2147483647;
        if (point->value.i32 < -2147483) return (-2147483647 - 1);
        return point->value.i32 * 1000;
    case GW_VALUE_F32: return (int32_t)(point->value.f32 * 1000.0f);
    case GW_VALUE_F64: return (int32_t)(point->value.f64 * 1000.0);
    default: return 0;
    }
}

static void m123_validation_task(void *argument)
{
    (void)argument;
    (void)xEventGroupWaitBits(g_system_events,
                              EVT_CONFIG_READY | EVT_RS485_READY,
                              pdFALSE, pdTRUE, portMAX_DELAY);

    bool m13_pass = false;
    bool m14_normal_pass = false;
    bool m14_retry_seen = false;
    bool m14_offline_seen = false;
    bool m14_recovery_seen = false;
    bool m14_pass = false;
    bool m2_pass = false;
    bool final_reported = false;
    uint64_t previous_point_ts = 0U;

    GW_LOGI("VAL", "M1.3/M1.4/M2 board validation monitor started");
    GW_LOGI("VAL", "phase A: keep slave online until M1.3+M2 PASS");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(GW_M123_VALIDATION_REPORT_MS));

        rs485_dma_stats_t dma;
        rs485_task_stats_t task_stats;
        gw_device_t dev;
        gw_point_t point;
        memset(&dma, 0, sizeof(dma));
        memset(&task_stats, 0, sizeof(task_stats));
        memset(&dev, 0, sizeof(dev));
        memset(&point, 0, sizeof(point));
        drv_rs485_get_stats(&dma);
        task_rs485_get_stats(&task_stats);

        bool have_dev = device_manager_get(1U, &dev) == GW_OK;
        bool have_point = point_db_get(1001U, &point) == GW_OK;

        GW_LOGI("VAL", "DMA start=%lu ftf=%lu tc=%lu rxIdle=%lu rxFtf=%lu dErr=%lu uErr=%lu",
                (unsigned long)dma.tx_dma_start_count,
                (unsigned long)dma.tx_dma_ftf_count,
                (unsigned long)dma.tx_tc_complete_count,
                (unsigned long)dma.rx_idle_count,
                (unsigned long)dma.rx_dma_ftf_count,
                (unsigned long)dma.dma_error_count,
                (unsigned long)dma.uart_error_count);
        GW_LOGI("VAL", "RTU attempts=%lu retries=%lu ok=%lu tout=%lu crc=%lu proto=%lu io=%lu",
                (unsigned long)task_stats.attempt_count,
                (unsigned long)task_stats.retry_count,
                (unsigned long)task_stats.final_ok_count,
                (unsigned long)task_stats.final_timeout_count,
                (unsigned long)task_stats.final_crc_count,
                (unsigned long)task_stats.final_protocol_count,
                (unsigned long)task_stats.final_io_count);

        if (have_dev && have_point) {
            GW_LOGI("VAL", "DEV=%s ok=%lu err=%lu consec=%lu POINT=%s v_x1000=%ld ts=%lu",
                    validation_device_state(dev.state),
                    (unsigned long)dev.success_count,
                    (unsigned long)dev.error_count,
                    (unsigned long)dev.consecutive_error,
                    validation_quality(point.quality),
                    (long)validation_value_x1000(&point),
                    (unsigned long)(uint32_t)point.timestamp_ms);
        }

        if (!m13_pass && have_dev &&
            (dev.success_count >= 3U) &&
            (dma.tx_dma_start_count >= 3U) &&
            (dma.tx_dma_ftf_count >= 3U) &&
            (dma.tx_tc_complete_count >= 3U) &&
            ((dma.rx_idle_count + dma.rx_dma_ftf_count) >= 3U)) {
            m13_pass = true;
            GW_LOGI("VAL", "M1.3 PASS: TX DMA -> UART TC -> RX DMA event observed");
        }

        if (!m14_normal_pass && m13_pass && have_dev &&
            (dev.state == DEVICE_ONLINE) && (dev.success_count >= 3U) &&
            (task_stats.final_ok_count >= 3U)) {
            m14_normal_pass = true;
            GW_LOGI("VAL", "M1.4 normal PASS: request/response transactions valid");
            GW_LOGI("VAL", "phase B: disconnect/power-off slave for >=5s to force retry+OFFLINE");
        }

        if ((task_stats.retry_count > 0U) && !m14_retry_seen) {
            m14_retry_seen = true;
            GW_LOGI("VAL", "M1.4 retry observed: PASS sub-gate");
        }

        if (have_dev && (dev.state == DEVICE_OFFLINE) && !m14_offline_seen) {
            m14_offline_seen = true;
            GW_LOGI("VAL", "M1.4 OFFLINE observed: PASS sub-gate; reconnect slave now");
        }

        if (m14_offline_seen && have_dev && have_point &&
            (dev.state == DEVICE_ONLINE) &&
            (point.quality == GW_QUALITY_GOOD) && !m14_recovery_seen) {
            m14_recovery_seen = true;
            GW_LOGI("VAL", "M1.4 recovery observed: ONLINE + GOOD");
        }

        if (!m2_pass && have_dev && have_point &&
            (dev.state == DEVICE_ONLINE) &&
            (point.quality == GW_QUALITY_GOOD) &&
            (previous_point_ts != 0U) &&
            (point.timestamp_ms > previous_point_ts)) {
            m2_pass = true;
            GW_LOGI("VAL", "M2 PASS: Poll Scheduler -> Device -> Point DB timestamp advances");
        }

        if (have_point && (point.timestamp_ms != 0U)) {
            previous_point_ts = point.timestamp_ms;
        }

        if (!m14_pass && m14_normal_pass && m14_retry_seen &&
            m14_offline_seen && m14_recovery_seen) {
            m14_pass = true;
            GW_LOGI("VAL", "M1.4 PASS: transaction/retry/offline/recovery closed loop");
        }

        if (m13_pass && m14_pass && m2_pass) {
            if (!final_reported) {
                final_reported = true;
                GW_LOGI("VAL", "FINAL PASS: M1.3=PASS M1.4=PASS M2=PASS");
            }
        } else {
            GW_LOGI("VAL", "STATUS M1.3=%s M1.4=%s M2=%s",
                    m13_pass ? "PASS" : "WAIT",
                    m14_pass ? "PASS" : "WAIT",
                    m2_pass ? "PASS" : "WAIT");
        }
    }
}

static void m123_validation_create(void)
{
    TaskHandle_t handle = xTaskCreateStatic(
        m123_validation_task, "m123val", M123_VAL_STACK_WORDS, NULL,
        M123_VAL_PRIORITY, s_m123_val_stack, &s_m123_val_task_cb);
    configASSERT(handle != NULL);
}
#endif

#if (GW_M3_BOARD_VALIDATION_ENABLE != 0)
#define M3_VAL_STACK_WORDS  896U
#define M3_VAL_PRIORITY     2U

static StaticTask_t s_m3_val_task_cb;
static StackType_t s_m3_val_stack[M3_VAL_STACK_WORDS];

static const char *m3_device_state(device_state_t state)
{
    switch (state) {
    case DEVICE_DISABLED: return "DISABLED";
    case DEVICE_INIT: return "INIT";
    case DEVICE_ONLINE: return "ONLINE";
    case DEVICE_OFFLINE: return "OFFLINE";
    case DEVICE_ERROR: return "ERROR";
    default: return "?";
    }
}

static const char *m3_quality(gw_quality_t quality)
{
    switch (quality) {
    case GW_QUALITY_GOOD: return "GOOD";
    case GW_QUALITY_STALE: return "STALE";
    case GW_QUALITY_TIMEOUT: return "TIMEOUT";
    case GW_QUALITY_BAD: return "BAD";
    case GW_QUALITY_OFFLINE: return "OFFLINE";
    case GW_QUALITY_INVALID: return "INVALID";
    default: return "?";
    }
}

static void m3_validation_task(void *argument)
{
    (void)argument;
    (void)xEventGroupWaitBits(g_system_events,
                              EVT_CONFIG_READY | EVT_CANFD_READY,
                              pdFALSE, pdTRUE, portMAX_DELAY);

    uint32_t tx_counter = 0U;
    uint64_t next_tx_ms = gw_time_ms();
    uint64_t next_report_ms = gw_time_ms();
    uint64_t previous_point_ts = 0U;
    bool rx_point_pass = false;
    bool tx_pass = false;
    bool offline_seen = false;
    bool recovery_seen = false;
    bool final_reported = false;

    GW_LOGI("M3VAL", "stable baseline: CAN-FD 500k BRS=OFF TDC=OFF");
    GW_LOGI("M3VAL", "PC send STD 0x%03X FD=1 BRS=0 LEN12, bytes[0..1]=00 FA",
            (unsigned)GW_CANFD_DEMO_RX_ID);
    GW_LOGI("M3VAL", "gateway sends STD 0x%03X FD=1 BRS=0 LEN12 every %lu ms",
            (unsigned)GW_CANFD_DEMO_TX_ID,
            (unsigned long)GW_CANFD_DEMO_TX_PERIOD_MS);

    for (;;) {
        uint64_t now = gw_time_ms();
        if (now >= next_tx_ms) {
            canfd_frame_t tx;
            memset(&tx, 0, sizeof(tx));
            tx.id = GW_CANFD_DEMO_TX_ID;
            tx.extended = false;
            tx.fd = true;
            tx.brs = false;
            tx.len = 12U;
            tx.data[0] = 0x47U; /* 'G' */
            tx.data[1] = 0x57U; /* 'W' */
            tx.data[2] = (uint8_t)(tx_counter >> 24U);
            tx.data[3] = (uint8_t)(tx_counter >> 16U);
            tx.data[4] = (uint8_t)(tx_counter >> 8U);
            tx.data[5] = (uint8_t)tx_counter;
            if (drv_canfd_submit(&tx, 0U)) {
                ++tx_counter;
            }
            next_tx_ms = now + GW_CANFD_DEMO_TX_PERIOD_MS;
        }

        if (now >= next_report_ms) {
            canfd_stats_t hw;
            can_decoder_stats_t dec;
            gw_device_t dev;
            gw_point_t point;
            memset(&hw, 0, sizeof(hw));
            memset(&dec, 0, sizeof(dec));
            memset(&dev, 0, sizeof(dev));
            memset(&point, 0, sizeof(point));
            drv_canfd_get_stats(&hw);
            can_decoder_get_stats(&dec);
            bool have_dev = device_manager_get(2U, &dev) == GW_OK;
            bool have_point = point_db_get(2001U, &point) == GW_OK;

            GW_LOGI("M3VAL", "CAN irq=%lu rxIRQ=%lu txIRQ=%lu spurTX=%lu rx=%lu fd=%lu brs=%lu ov=%lu drop=%lu txGood=%lu txFail=%lu boff=%lu rec=%lu",
                    (unsigned long)hw.message_irq_count,
                    (unsigned long)hw.rx_irq_count,
                    (unsigned long)hw.tx_irq_count,
                    (unsigned long)hw.tx_spurious_irq_count,
                    (unsigned long)hw.rx_frames,
                    (unsigned long)hw.rx_fd_frames,
                    (unsigned long)hw.rx_brs_frames,
                    (unsigned long)hw.rx_overrun,
                    (unsigned long)hw.rx_queue_drop,
                    (unsigned long)hw.tx_success,
                    (unsigned long)hw.tx_failed,
                    (unsigned long)hw.busoff_count,
                    (unsigned long)hw.busoff_recovery_count);
            GW_LOGI("M3VAL", "ERR state=%lu TEC=%lu REC=%lu fdTEC=%lu fdREC=%lu ack=%lu stuff=%lu form=%lu crc=%lu bit=%lu",
                    (unsigned long)hw.error_state,
                    (unsigned long)hw.tx_error_count,
                    (unsigned long)hw.rx_error_count,
                    (unsigned long)hw.fd_tx_error_count,
                    (unsigned long)hw.fd_rx_error_count,
                    (unsigned long)hw.ack_error_count,
                    (unsigned long)hw.stuff_error_count,
                    (unsigned long)hw.form_error_count,
                    (unsigned long)hw.crc_error_count,
                    (unsigned long)hw.bit_error_count);
            GW_LOGI("M3VAL", "DEC seen=%lu match=%lu sig=%lu pointQ=%lu drop=%lu lenErr=%lu schema=%lu stale=%lu off=%lu",
                    (unsigned long)dec.frames_seen,
                    (unsigned long)dec.frames_matched,
                    (unsigned long)dec.signals_decoded,
                    (unsigned long)dec.point_updates_queued,
                    (unsigned long)dec.point_update_drop,
                    (unsigned long)dec.invalid_length,
                    (unsigned long)dec.schema_error,
                    (unsigned long)dec.stale_events,
                    (unsigned long)dec.offline_events);
            if (have_dev && have_point) {
                GW_LOGI("M3VAL", "DEV=%s ok=%lu err=%lu consec=%lu POINT=%s value_x10=%ld rev=%lu ts=%lu",
                        m3_device_state(dev.state),
                        (unsigned long)dev.success_count,
                        (unsigned long)dev.error_count,
                        (unsigned long)dev.consecutive_error,
                        m3_quality(point.quality),
                        (long)(point.value.f32 * 10.0f),
                        (unsigned long)point.revision,
                        (unsigned long)(uint32_t)point.timestamp_ms);
            }

            if (!tx_pass && hw.tx_success >= 3U && hw.tx_failed == 0U &&
                hw.tx_spurious_irq_count == 0U && hw.tx_error_count == 0U &&
                hw.fd_tx_error_count == 0U) {
                tx_pass = true;
                GW_LOGI("M3VAL", "TX PASS: CAN-FD BRS-OFF queue -> mailbox -> completion");
            }
            if (!rx_point_pass && have_dev && have_point &&
                hw.rx_fd_frames >= 3U && hw.rx_brs_frames == 0U &&
                dec.signals_decoded >= 3U &&
                point.quality == GW_QUALITY_GOOD &&
                previous_point_ts != 0U && point.timestamp_ms > previous_point_ts) {
                rx_point_pass = true;
                GW_LOGI("M3VAL", "RX/Point PASS: ISR mailbox -> q_can_rx -> decoder -> Point DB");
                GW_LOGI("M3VAL", "phase B optional: stop CAN ID 0x%03X for >=5s, then resume",
                        (unsigned)GW_CANFD_DEMO_RX_ID);
            }
            if (have_dev && dev.state == DEVICE_OFFLINE && !offline_seen) {
                offline_seen = true;
                GW_LOGI("M3VAL", "CAN device OFFLINE observed; resume sender now");
            }
            if (offline_seen && have_dev && have_point &&
                dev.state == DEVICE_ONLINE && point.quality == GW_QUALITY_GOOD &&
                !recovery_seen) {
                recovery_seen = true;
                GW_LOGI("M3VAL", "CAN device recovery observed: ONLINE + GOOD");
            }
            if (have_point && point.timestamp_ms != 0U) {
                previous_point_ts = point.timestamp_ms;
            }
            if (tx_pass && rx_point_pass && offline_seen && recovery_seen) {
                if (!final_reported) {
                    final_reported = true;
                    GW_LOGI("M3VAL", "STABLE PASS: CAN-FD 500k BRS-OFF + Point DB closed loop");
                }
            } else {
                GW_LOGI("M3VAL", "STATUS TX=%s RXPOINT=%s OFFLINE=%s RECOVERY=%s",
                        tx_pass ? "PASS" : "WAIT",
                        rx_point_pass ? "PASS" : "WAIT",
                        offline_seen ? "PASS" : "WAIT",
                        recovery_seen ? "PASS" : "WAIT");
            }
            next_report_ms = now + GW_M3_VALIDATION_REPORT_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
}

static void m3_validation_create(void)
{
    TaskHandle_t handle = xTaskCreateStatic(
        m3_validation_task, "m3val", M3_VAL_STACK_WORDS, NULL,
        M3_VAL_PRIORITY, s_m3_val_stack, &s_m3_val_task_cb);
    configASSERT(handle != NULL);
}
#endif

#if (GW_DEMO_MODBUS_CONFIG_ENABLE != 0U)
static void register_modbus_demo_objects(void)
{
    gw_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.id = 1U;
    memcpy(dev.name, "Meter01", sizeof("Meter01"));
    dev.protocol = GW_PROTO_MODBUS_RTU;
    dev.interface_id = GW_IF_RS485_0;
    dev.address = GW_RS485_TEST_SLAVE;
    dev.timeout_ms = GW_RS485_RESPONSE_TIMEOUT_MS;
    dev.retry = GW_RS485_RETRY_COUNT;
    dev.state = DEVICE_INIT;
    configASSERT(device_manager_register(&dev) == GW_OK);

    gw_point_t point;
    memset(&point, 0, sizeof(point));
    point.id = 1001U;
    point.device_id = dev.id;
    memcpy(point.name, "Voltage_A", sizeof("Voltage_A"));
    point.type = GW_VALUE_F32;
    point.scale = 0.1f;
    point.offset = 0.0f;
    point.quality = GW_QUALITY_STALE;
    configASSERT(point_db_register(&point) == GW_OK);

    poll_job_t poll;
    memset(&poll, 0, sizeof(poll));
    poll.id = 1U;
    poll.device_id = dev.id;
    poll.point_id = point.id;
    poll.function_code = 0x03U;
    poll.start_address = GW_RS485_TEST_ADDRESS;
    poll.quantity = GW_RS485_TEST_QUANTITY;
    poll.register_offset = 0U;
    poll.interval_ms = GW_RS485_TEST_PERIOD_MS;
    poll.encoding = POLL_ENCODING_U16;
    poll.enabled = true;
    configASSERT(poll_scheduler_register(&poll) == GW_OK);
}
#endif

#if ((GW_CANFD_ENABLE != 0) && (GW_DEMO_CAN_CONFIG_ENABLE != 0U))
static void register_can_demo_objects(void)
{
    gw_device_t can_dev;
    memset(&can_dev, 0, sizeof(can_dev));
    can_dev.id = 2U;
    memcpy(can_dev.name, "CANFD01", sizeof("CANFD01"));
    can_dev.protocol = GW_PROTO_CAN;
    can_dev.interface_id = GW_IF_CANFD_0;
    can_dev.address = 0U;
    can_dev.timeout_ms = 1500U;
    can_dev.retry = 0U;
    can_dev.state = DEVICE_INIT;
    configASSERT(device_manager_register(&can_dev) == GW_OK);

    gw_point_t can_point;
    memset(&can_point, 0, sizeof(can_point));
    can_point.id = 2001U;
    can_point.device_id = can_dev.id;
    memcpy(can_point.name, "CANFD_Value", sizeof("CANFD_Value"));
    can_point.type = GW_VALUE_F32;
    can_point.scale = 0.1f;
    can_point.offset = 0.0f;
    can_point.quality = GW_QUALITY_STALE;
    configASSERT(point_db_register(&can_point) == GW_OK);

    can_signal_map_t map;
    memset(&map, 0, sizeof(map));
    map.id = 1U;
    map.device_id = can_dev.id;
    map.point_id = can_point.id;
    map.can_id = GW_CANFD_DEMO_RX_ID;
    map.byte_offset = 0U;
    map.encoding = CAN_SIGNAL_U16;
    map.endian = CAN_ENDIAN_BIG;
    map.extended = false;
    map.require_fd = true;
    map.enabled = true;
    configASSERT(can_decoder_register(&map) == GW_OK);
}
#endif

void gateway_app_init(void)
{
    /* v0.9 initialization contract:
     * 1) static pools/RTOS primitives
     * 2) empty southbound databases
     * 3) service state and persistent runtime configuration
     * 4) task creation
     * 5) watchdog last, after every monitored task exists. */
    gw_msg_pool_init();
    rtos_objects_init();

    point_db_init();
    device_manager_init();
    poll_scheduler_init();
    can_decoder_init();
    gw_command_router_init();
    gw_uplink_init();
    gw_syslog_init();
    gw_diagnostics_init();
    gw_sntp_init();
    gw_snmp_init();
    gw_ota_init();
    gw_watchdog_init();

    /* Persistent config may rebuild Device/Point/CAN/Poll/Alarm/Rule tables.
     * This must happen before CONFIG_READY becomes visible to worker tasks. */
    gw_config_init();

#if (GW_DEMO_MODBUS_CONFIG_ENABLE != 0U)
    register_modbus_demo_objects();
#endif
#if ((GW_CANFD_ENABLE != 0) && (GW_DEMO_CAN_CONFIG_ENABLE != 0U))
    register_can_demo_objects();
#endif

    (void)xEventGroupSetBits(g_system_events, EVT_CONFIG_READY);

    gw_config_task_create();
    task_rs485_create();
    task_data_create();
#if (GW_CANFD_ENABLE != 0)
    task_can_create();
#endif
    poll_scheduler_create();
#if (GW_ETH_ENABLE != 0U)
    gw_net_task_create();
#if (GW_TCP_SERVER_ENABLE != 0U)
    gw_tcp_server_task_create();
#endif
#if (GW_UPLINK_ENABLE != 0U)
    gw_uplink_task_create();
#endif
#if (GW_SNTP_ENABLE != 0U)
    gw_sntp_task_create();
#endif
#if (GW_SYSLOG_ENABLE != 0U)
    gw_syslog_task_create();
#endif
#if (GW_SNMP_ENABLE != 0U)
    gw_snmp_task_create();
#endif
#endif
#if (GW_DIAGNOSTICS_ENABLE != 0U)
    gw_diagnostics_task_create();
#endif
#if (GW_GUI_ENABLE != 0U)
    gw_gui_task_create();
#endif
    rs485_smoke_test_task_create();
#if (GW_M123_BOARD_VALIDATION_ENABLE != 0)
    m123_validation_create();
#endif
#if (GW_M3_BOARD_VALIDATION_ENABLE != 0)
    m3_validation_create();
#endif

    (void)xEventGroupSetBits(g_system_events, EVT_SYSTEM_RUNNING);
#if (GW_WATCHDOG_ENABLE != 0U)
    gw_watchdog_task_create();
#endif
    GW_LOGI("SYS", "gateway v0.9 services initialized: runtime config + time + automation + ops + spool + security + HMI");
}
