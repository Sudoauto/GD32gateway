#ifndef GATEWAY_BUILD_CONFIG_H
#define GATEWAY_BUILD_CONFIG_H

/* Board/application bring-up configuration. */
#define GW_DEBUG_BAUDRATE                 115200U

/* Ethernet baseline: ENET0 + LAN8720A over RMII.
 * PHY address is 0. v0.7.0 GUI reserves PA8 for TLI_R6, therefore the full
 * Ethernet+LCD build expects an external 50 MHz RMII PHY reference clock.
 * Network stack starts in a dedicated FreeRTOS task so boot remains healthy
 * when the cable is unplugged. */
#define GW_ETH_ENABLE                     1U

/* v0.7.0 5-inch LVGL GUI baseline.
 * The official TLI/RGB pin map uses PA8 as LCD R6.  v0.6.x Ethernet used
 * PA8/CKOUT0 to generate the LAN8720A 50 MHz RMII reference, so full-color
 * LCD + Ethernet cannot share the original PA8 clock scheme.  For the GUI
 * baseline the PHY must receive its 50 MHz reference from the board's
 * external oscillator / equivalent hardware source, leaving PA8 to TLI_R6. */
#define GW_GUI_ENABLE                     1U
#define GW_GUI_REFRESH_MS                 250U
#define GW_GUI_TASK_PRIORITY              2U
#define GW_GUI_TASK_STACK_WORDS           3072U
#define GW_GUI_DIAGNOSTIC_LOG             0U
#define GW_GUI_DIAG_PERIOD_MS             5000U

/* RMII reference clock ownership. Exactly one source must be selected.
 * PA8 MCO is retained as an option for headless builds; GUI full-color mode
 * requires EXTERNAL_50M=1 and PA8_MCO=0. */
#define GW_ETH_RMII_REFCLK_PA8_MCO        0U
#define GW_ETH_RMII_REFCLK_EXTERNAL_50M   1U
#define GW_ETH_PHY_ADDRESS                0U
#define GW_ETH_IRQ_PREEMPT_PRIORITY       4U
#define GW_ETH_DHCP_ENABLE                0U
#define GW_ETH_DIAGNOSTIC_LOG             1U
#define GW_ETH_DIAG_PERIOD_MS             5000U

/* v0.6.1 TCP communication baseline.  A single-client Netconn echo server is
 * kept deliberately small and low priority so CAN/RS485 real-time paths remain
 * dominant.  Accept/receive calls are bounded by timeouts so link-down and IP
 * loss can always unwind without blocking the gateway indefinitely. */
#define GW_TCP_SERVER_ENABLE              1U
#define GW_TCP_SERVER_PORT                5000U
#define GW_TCP_SERVER_BACKLOG             2U
#define GW_TCP_ACCEPT_POLL_MS             500U
#define GW_TCP_RECV_POLL_MS               500U
#define GW_TCP_SEND_TIMEOUT_MS            1000U
#define GW_TCP_RETRY_MS                   1000U
#define GW_TCP_DIAGNOSTIC_LOG             1U
#define GW_TCP_DIAG_PERIOD_MS             5000U
#define GW_TCP_ECHO_ENABLE                1U

/* v0.8.0 normalized northbound stream. Port 5000 remains the validated TCP
 * echo/transport service; port 5001 publishes newline-delimited JSON records
 * for parsed points and raw CAN/Modbus frames using one common envelope. */
#define GW_UPLINK_ENABLE                  1U
#define GW_UPLINK_PORT                    5001U


/* v0.9.0 edge-management baseline ------------------------------------------------
 * Runtime configuration is loaded from persistent CSV.  The final 256 KiB of
 * internal Flash is reserved by the Keil target for two atomic config slots and
 * an offline event spool.  SD/OSPI can be plugged in through the storage port
 * without changing the config service. */
#define GW_RUNTIME_CONFIG_ENABLE          1U
#define GW_CONFIG_AUTOSAVE_ENABLE         1U
#define GW_CONFIG_AUTOSAVE_DELAY_MS       1500U
#define GW_CONFIG_MAX_CSV_BYTES           28672U
#define GW_CONFIG_FACTORY_RESET_REBOOT_MS 300U

/* UTC synchronization. SNTP is the production baseline; PTP is intentionally
 * disabled until a hardware-timestamped PHY/board path is validated. */
#define GW_SNTP_ENABLE                    1U
#define GW_SNTP_SERVER                    "pool.ntp.org"
#define GW_SNTP_SYNC_PERIOD_MS            3600000U
#define GW_SNTP_STARTUP_RETRY_MS          10000U
#define GW_PTP_ENABLE                     0U

/* Local intelligence. Alarm events use a dedicated high-priority uplink queue;
 * rules execute through the same command router used by HMI and northbound. */
#define GW_ALARM_ENABLE                   1U
#define GW_RULE_ENGINE_ENABLE             1U
#define GW_MAX_ALARM_RULES                64U
#define GW_MAX_LINKAGE_RULES              32U
#define GW_RULE_MIN_RETRIGGER_MS           1000U

/* Runtime supervision. The FWDGT is only refreshed when all enabled mandatory
 * services have reported progress inside their deadline. */
#define GW_WATCHDOG_ENABLE                1U
#define GW_WATCHDOG_START_GRACE_MS        8000U
#define GW_WATCHDOG_TASK_PERIOD_MS        250U
#define GW_WATCHDOG_STALE_MS              4000U
#define GW_WATCHDOG_TIMEOUT_MS            6000U

/* Remote operations. Syslog sends warning/error records via UDP/514. The
 * compact SNMPv2c agent exposes private read-only health OIDs without pulling
 * the full lwIP SNMP application into every build. */
#define GW_SYSLOG_ENABLE                  1U
#define GW_SYSLOG_DEFAULT_SERVER          "192.168.103.19"
#define GW_SYSLOG_DEFAULT_PORT            514U
#define GW_SNMP_ENABLE                    1U
#define GW_SNMP_PORT                      161U
#define GW_SNMP_COMMUNITY                 "public"

/* Offline black-box spool. JSONL records are committed to persistent Flash when
 * the northbound client is absent and replayed oldest-first after reconnect. */
#define GW_OFFLINE_SPOOL_ENABLE           1U
#define GW_OFFLINE_REPLAY_BURST           4U

/* Security baseline. Credentials are salted SHA-256 in persistent config.
 * Factory credentials MUST be changed before deployment. OTA is fail-closed:
 * a signed manifest and platform signature backend are mandatory before an
 * image can be committed. */
#define GW_AUTH_ENABLE                    1U
#define GW_AUTH_DEFAULT_USER              "admin"
#define GW_AUTH_DEFAULT_PASSWORD          "ChangeMe123!"
#define GW_AUTH_SESSION_IDLE_MS           900000U
#define GW_AUTH_MAX_FAILURES              5U
#define GW_AUTH_LOCKOUT_MS                60000U
#define GW_OTA_ENABLE                     1U
#define GW_SECURE_BOOT_REQUIRED           1U
#define GW_PRODUCTION_LOCK_ENABLE         0U
#define GW_PRODUCTION_LOCK_CONFIRM        0x00000000UL

/* Diagnostics / self test. Differential bus lines must never be shorted
 * together as a test method; the self-test service uses controller/software
 * checks and an explicit external loopback fixture where required. */
#define GW_DIAGNOSTICS_ENABLE             1U
#define GW_DIAG_HISTORY_DEPTH             120U
#define GW_DIAG_SAMPLE_MS                 1000U
#define GW_SELFTEST_ENABLE                1U

/* Touch robustness: bound every I2C transaction, recover on controller error,
 * reprobe/reset after consecutive failures, and never block the LVGL task. */
#define GW_TOUCH_IO_TIMEOUT_MS            4U
#define GW_TOUCH_REPROBE_THRESHOLD        3U
#define GW_TOUCH_REPROBE_BACKOFF_MS       250U

/* Locally administered MAC for bring-up. Change before shipping fleets. */
#define GW_ETH_MAC0                       0x02U
#define GW_ETH_MAC1                       0x47U
#define GW_ETH_MAC2                       0x44U
#define GW_ETH_MAC3                       0x32U
#define GW_ETH_MAC4                       0x48U
#define GW_ETH_MAC5                       0x01U

/* Static IPv4 baseline when DHCP is disabled. Mirrors the supplied example's
 * 192.168.103.0/24 lab network so initial ping/TCP verification is direct. */
#define GW_ETH_IP0                        192U
#define GW_ETH_IP1                        168U
#define GW_ETH_IP2                        103U
#define GW_ETH_IP3                        213U
#define GW_ETH_MASK0                      255U
#define GW_ETH_MASK1                      255U
#define GW_ETH_MASK2                      255U
#define GW_ETH_MASK3                      0U
#define GW_ETH_GW0                        192U
#define GW_ETH_GW1                        168U
#define GW_ETH_GW2                        103U
#define GW_ETH_GW3                        254U

#define GW_RS485_BAUDRATE                 9600U
#define GW_RS485_DATA_BITS                8U
#define GW_RS485_STOP_BITS                1U
#define GW_RS485_PARITY                   RS485_PARITY_NONE
#define GW_RS485_RESPONSE_TIMEOUT_MS      300U
#define GW_RS485_RETRY_COUNT              1U

/* M1/M2 are frozen after hardware validation. Keep transport DMA-only, but
 * disable verbose validation diagnostics in the M3 branch. */
#define GW_RS485_TX_DIAGNOSTIC_LOG        0
#define GW_RS485_RX_DIAGNOSTIC_LOG        0
#define GW_M123_BOARD_VALIDATION_ENABLE   0
#define GW_M123_VALIDATION_REPORT_MS      2000U

/* Legacy Modbus smoke test remains disabled; Poll Scheduler is the owner. */
#define GW_RS485_SMOKE_TEST_ENABLE        0
#define GW_RS485_TEST_SLAVE               1U
#define GW_RS485_TEST_ADDRESS             0U
#define GW_RS485_TEST_QUANTITY            2U
#define GW_RS485_TEST_PERIOD_MS           1000U

/* Stable CAN-FD baseline.
 *
 * The production baseline keeps CAN-FD payload capability but disables bit-rate
 * switching.  The whole frame therefore remains at 500 kbit/s.  This removes
 * the high-speed data-phase/termination variable while preserving up to 64-byte
 * ISO CAN-FD payloads.
 *
 * Historical high-speed BRS stages are intentionally not selectable from this
 * production baseline. They remain documented in the archived validation notes.
 */
#define GW_CANFD_ENABLE                   1
#define GW_CANFD_BRS_ENABLE               0U
#define GW_CANFD_TDC_ACTIVE               0U

/* GD32H73x/H75x erratum 2.15.7 recommends CK_CAN = CK_APB2.  In this project
 * SYSCLK=600 MHz, AHB=300 MHz and APB2=300 MHz, hence CK_CAN=300 MHz.
 * 300M / (60 * (1 + 2 + 5 + 2)) = 500 kbit/s, sample point = 80%. */
#define GW_CANFD_NOMINAL_PRESCALER        60U
#define GW_CANFD_NOMINAL_SJW              1U
#define GW_CANFD_NOMINAL_PROP_SEG         2U
#define GW_CANFD_NOMINAL_SEG1             5U
#define GW_CANFD_NOMINAL_SEG2             2U

/* With BRS disabled the data phase never switches away from the nominal bit
 * timing.  Keep the FD data timing equal to the nominal timing as a defensive
 * configuration. Re-introducing BRS requires a separate reviewed branch; this
 * stable baseline intentionally fails compilation if BRS is enabled. */
#define GW_CANFD_DATA_PRESCALER           60U
#define GW_CANFD_DATA_SJW                 1U
#define GW_CANFD_DATA_PROP_SEG            2U
#define GW_CANFD_DATA_SEG1                5U
#define GW_CANFD_DATA_SEG2                2U
#define GW_CANFD_TDC_OFFSET               0U

/* Generic TX protection remains available to the driver.  In the stable BRS
 * OFF baseline only nominal TEC is relevant; fdTEC must remain zero. */
#define GW_CANFD_TX_HOLD_TEC_THRESHOLD     96U
#define GW_CANFD_TX_HOLD_FD_TEC_THRESHOLD  8U

/* Production defaults: no automatic validation traffic, no demo devices/polls,
 * and no per-frame RX tracing.  These may be enabled explicitly for board
 * bring-up, but should remain OFF before northbound protocol integration. */
#define GW_DEMO_MODBUS_CONFIG_ENABLE      0U
#define GW_DEMO_CAN_CONFIG_ENABLE         0U
#define GW_M3_BOARD_VALIDATION_ENABLE     0U
#define GW_M3_VALIDATION_REPORT_MS        1000U
#define GW_CANFD_RX_TRACE_ENABLE          0U
#define GW_CANFD_RX_TRACE_FIRST_N         6U
#define GW_CANFD_RX_TRACE_EVERY_N         20U
#define GW_CANFD_ERROR_IRQ_ENABLE         0U
#define GW_CANFD_DEMO_RX_ID               0x301U
#define GW_CANFD_DEMO_TX_ID               0x302U
#define GW_CANFD_DEMO_TX_PERIOD_MS        1000U

/* ISR priorities that call FreeRTOS FromISR APIs must be numerically >=
 * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (=2). CAN RX uses the highest
 * RTOS-safe priority so the GD32H7 receive mailbox is drained promptly per
 * erratum 2.15.6. RS485 remains at priority 5. */
#define GW_CANFD_IRQ_PREEMPT_PRIORITY     2U
#define GW_RTOS_IRQ_PREEMPT_PRIORITY      5U
#define GW_RTOS_IRQ_SUB_PRIORITY          0U

#if (GW_RS485_BAUDRATE == 0U)
#error "GW_RS485_BAUDRATE must be non-zero"
#endif
#if (GW_RS485_DATA_BITS != 8U)
#error "This RS485 driver currently supports 8 data bits only"
#endif
#if ((GW_RS485_STOP_BITS != 1U) && (GW_RS485_STOP_BITS != 2U))
#error "GW_RS485_STOP_BITS must be 1 or 2"
#endif
#if (GW_RS485_RESPONSE_TIMEOUT_MS == 0U)
#error "GW_RS485_RESPONSE_TIMEOUT_MS must be non-zero"
#endif
#if ((GW_RS485_TEST_SLAVE == 0U) || (GW_RS485_TEST_SLAVE > 247U))
#error "GW_RS485_TEST_SLAVE must be 1..247"
#endif
#if ((GW_RS485_TEST_QUANTITY == 0U) || (GW_RS485_TEST_QUANTITY > 125U))
#error "GW_RS485_TEST_QUANTITY must be 1..125"
#endif
#if (GW_RS485_TEST_PERIOD_MS == 0U)
#error "GW_RS485_TEST_PERIOD_MS must be non-zero"
#endif

#if (GW_CANFD_ENABLE != 0)
#if (GW_CANFD_BRS_ENABLE != 0U)
#error "v0.5.0 stable baseline requires CAN-FD BRS disabled"
#endif
#if ((GW_CANFD_BRS_ENABLE == 0U) && (GW_CANFD_TDC_ACTIVE != 0U))
#error "TDC is not used when BRS is disabled"
#endif
#if ((GW_CANFD_NOMINAL_PRESCALER == 0U) || (GW_CANFD_NOMINAL_SJW == 0U) || \
     (GW_CANFD_NOMINAL_PROP_SEG == 0U) || (GW_CANFD_NOMINAL_SEG1 == 0U) || \
     (GW_CANFD_NOMINAL_SEG2 == 0U))
#error "CAN-FD nominal timing fields must be non-zero"
#endif
#if ((GW_CANFD_DATA_PRESCALER == 0U) || (GW_CANFD_DATA_PRESCALER > 1024U) || \
     (GW_CANFD_DATA_SJW == 0U) || (GW_CANFD_DATA_SJW > 8U) || \
     (GW_CANFD_DATA_PROP_SEG > 31U) || \
     (GW_CANFD_DATA_SEG1 == 0U) || (GW_CANFD_DATA_SEG1 > 8U) || \
     (GW_CANFD_DATA_SEG2 < 2U) || (GW_CANFD_DATA_SEG2 > 8U) || \
     (GW_CANFD_DATA_SJW > GW_CANFD_DATA_SEG2))
#error "CAN-FD data timing fields are outside GD32H7 SPL limits"
#endif
#if ((GW_CANFD_BRS_ENABLE == 0U) && \
     ((GW_CANFD_DATA_PRESCALER != GW_CANFD_NOMINAL_PRESCALER) || \
      (GW_CANFD_DATA_SJW != GW_CANFD_NOMINAL_SJW) || \
      (GW_CANFD_DATA_PROP_SEG != GW_CANFD_NOMINAL_PROP_SEG) || \
      (GW_CANFD_DATA_SEG1 != GW_CANFD_NOMINAL_SEG1) || \
      (GW_CANFD_DATA_SEG2 != GW_CANFD_NOMINAL_SEG2)))
#error "BRS-OFF baseline requires FD data timing to equal nominal 500k timing"
#endif
#if ((GW_CANFD_TDC_ACTIVE != 0U) && (GW_CANFD_TDC_OFFSET > 31U))
#error "CAN-FD TDC offset must be 0..31"
#endif
#if ((GW_CANFD_DEMO_RX_ID > 0x7FFU) || (GW_CANFD_DEMO_TX_ID > 0x7FFU))
#error "M3 demo uses standard 11-bit CAN identifiers"
#endif
#if ((GW_M3_BOARD_VALIDATION_ENABLE != 0U) && (GW_DEMO_CAN_CONFIG_ENABLE == 0U))
#error "M3 validation requires CAN demo device/point/map to be enabled"
#endif
#if ((GW_M123_BOARD_VALIDATION_ENABLE != 0U) && (GW_DEMO_MODBUS_CONFIG_ENABLE == 0U))
#error "M1/M2 validation requires Modbus demo device/point/poll to be enabled"
#endif
#endif

#if (GW_GUI_ENABLE != 0U)
#if ((GW_GUI_REFRESH_MS == 0U) || (GW_GUI_TASK_STACK_WORDS < 2048U))
#error "GUI refresh must be non-zero and GUI task stack must be >= 2048 words"
#endif
#if ((GW_GUI_DIAGNOSTIC_LOG != 0U) && (GW_GUI_DIAG_PERIOD_MS == 0U))
#error "GUI diagnostic period must be non-zero when diagnostics are enabled"
#endif
#if (GW_GUI_TASK_PRIORITY >= 8U)
#error "GUI task priority must be lower than FreeRTOS configMAX_PRIORITIES (8)"
#endif
#endif

#if (GW_ETH_ENABLE != 0U)
#if ((GW_ETH_RMII_REFCLK_PA8_MCO + GW_ETH_RMII_REFCLK_EXTERNAL_50M) != 1U)
#error "Select exactly one RMII 50 MHz reference-clock source"
#endif
#if ((GW_GUI_ENABLE != 0U) && (GW_ETH_RMII_REFCLK_PA8_MCO != 0U))
#error "PA8 conflict: LCD TLI_R6 and Ethernet CKOUT0 cannot coexist; use external PHY 50 MHz RMII clock"
#endif
#if (GW_ETH_PHY_ADDRESS > 31U)
#error "GW_ETH_PHY_ADDRESS must be 0..31"
#endif
#if ((GW_ETH_IRQ_PREEMPT_PRIORITY < 2U) || (GW_ETH_IRQ_PREEMPT_PRIORITY > 15U))
#error "Ethernet IRQ priority must be FreeRTOS-safe (2..15)"
#endif
#if ((GW_ETH_MAC0 & 0x01U) != 0U)
#error "Ethernet MAC must be unicast"
#endif
#if (GW_TCP_SERVER_ENABLE != 0U)
#if ((GW_TCP_SERVER_PORT == 0U) || (GW_TCP_SERVER_PORT > 65535U))
#error "GW_TCP_SERVER_PORT must be 1..65535"
#endif
#if ((GW_TCP_SERVER_BACKLOG == 0U) || (GW_TCP_SERVER_BACKLOG > 16U))
#error "GW_TCP_SERVER_BACKLOG must be 1..16"
#endif
#if ((GW_TCP_ACCEPT_POLL_MS == 0U) || (GW_TCP_RECV_POLL_MS == 0U) || \
     (GW_TCP_SEND_TIMEOUT_MS == 0U) || (GW_TCP_RETRY_MS == 0U))
#error "TCP baseline timeouts must be non-zero"
#endif
#endif
#if (GW_UPLINK_ENABLE != 0U)
#if ((GW_UPLINK_PORT == 0U) || (GW_UPLINK_PORT > 65535U))
#error "GW_UPLINK_PORT must be 1..65535"
#endif
#if ((GW_TCP_SERVER_ENABLE != 0U) && (GW_UPLINK_PORT == GW_TCP_SERVER_PORT))
#error "GW_UPLINK_PORT must not collide with GW_TCP_SERVER_PORT"
#endif
#endif
#endif

#endif
