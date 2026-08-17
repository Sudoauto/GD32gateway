#include "gw_tcp_server.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

#include "gateway_build_config.h"
#include "gw_log.h"
#include "rtos_objects.h"

#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"

#if ((GW_ETH_ENABLE != 0U) && (GW_TCP_SERVER_ENABLE != 0U))

#define TCP_SERVER_TASK_STACK_WORDS  1024U
#define TCP_SERVER_TASK_PRIORITY     2U
#define TCP_SERVER_RX_BURST_LIMIT    8U

static StaticTask_t s_tcp_server_tcb;
static StackType_t s_tcp_server_stack[TCP_SERVER_TASK_STACK_WORDS];
static gw_tcp_server_stats_t s_stats;

static void log_stats(void);

static void stats_set_listening(bool listening)
{
    taskENTER_CRITICAL();
    s_stats.listening = listening;
    taskEXIT_CRITICAL();
}

static void stats_set_client(bool connected)
{
    taskENTER_CRITICAL();
    s_stats.client_connected = connected;
    taskEXIT_CRITICAL();
}

static bool network_ready(void)
{
    EventBits_t bits = xEventGroupGetBits(g_system_events);
    return (bits & EVT_NET_IP_READY) != 0U;
}

static void close_client(struct netconn **client)
{
    if ((client == NULL) || (*client == NULL)) {
        return;
    }

    (void)netconn_close(*client);
    (void)netconn_delete(*client);
    *client = NULL;
    stats_set_client(false);
    (void)xEventGroupClearBits(g_system_events, EVT_TCP_CLIENT_CONNECTED);
}

static void delete_listener(struct netconn **listener)
{
    if ((listener == NULL) || (*listener == NULL)) {
        return;
    }

    (void)netconn_delete(*listener);
    *listener = NULL;
    stats_set_listening(false);
    (void)xEventGroupClearBits(g_system_events, EVT_TCP_SERVER_READY);
}

static err_t send_all(struct netconn *client, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0U;

    if ((client == NULL) || ((data == NULL) && (len != 0U))) {
        return ERR_ARG;
    }

    while (offset < len) {
        size_t written = 0U;
        err_t err = netconn_write_partly(client, &bytes[offset], len - offset,
                                         NETCONN_COPY, &written);
        if (err != ERR_OK) {
            taskENTER_CRITICAL();
            ++s_stats.send_error_count;
            taskEXIT_CRITICAL();
            return err;
        }
        if (written == 0U) {
            taskENTER_CRITICAL();
            ++s_stats.send_error_count;
            taskEXIT_CRITICAL();
            return ERR_IF;
        }

        offset += written;
        taskENTER_CRITICAL();
        ++s_stats.send_count;
        s_stats.tx_bytes += (uint32_t)written;
        taskEXIT_CRITICAL();
    }

    return ERR_OK;
}

static bool echo_netbuf(struct netconn *client, struct netbuf *buf)
{
    if ((client == NULL) || (buf == NULL)) {
        return false;
    }

    taskENTER_CRITICAL();
    ++s_stats.recv_count;
    s_stats.rx_bytes += (uint32_t)netbuf_len(buf);
    taskEXIT_CRITICAL();

    netbuf_first(buf);
    for (;;) {
        void *data = NULL;
        u16_t len = 0U;
        err_t err = netbuf_data(buf, &data, &len);
        if (err != ERR_OK) {
            taskENTER_CRITICAL();
            ++s_stats.recv_error_count;
            taskEXIT_CRITICAL();
            return false;
        }

#if (GW_TCP_ECHO_ENABLE != 0U)
        if ((len != 0U) && (send_all(client, data, (size_t)len) != ERR_OK)) {
            return false;
        }
#else
        (void)data;
        (void)len;
#endif

        if (netbuf_next(buf) < 0) {
            break;
        }
    }
    return true;
}

static void log_peer(struct netconn *client)
{
    ip_addr_t peer;
    u16_t port = 0U;
    char addr_text[IPADDR_STRLEN_MAX];

    memset(&peer, 0, sizeof(peer));
    memset(addr_text, 0, sizeof(addr_text));
    if ((client != NULL) &&
        (netconn_peer(client, &peer, &port) == ERR_OK) &&
        (ipaddr_ntoa_r(&peer, addr_text, (int)sizeof(addr_text)) != NULL)) {
        GW_LOGI("TCP", "client connected %s:%u", addr_text, (unsigned)port);
    } else {
        GW_LOGI("TCP", "client connected");
    }
}

static void serve_client(struct netconn *client)
{
    if (client == NULL) {
        return;
    }

    netconn_set_recvtimeout(client, (int)GW_TCP_RECV_POLL_MS);
    netconn_set_sendtimeout(client, (int)GW_TCP_SEND_TIMEOUT_MS);
    stats_set_client(true);
    (void)xEventGroupSetBits(g_system_events, EVT_TCP_CLIENT_CONNECTED);
    taskENTER_CRITICAL();
    ++s_stats.accept_count;
    taskEXIT_CRITICAL();
    log_peer(client);

    uint32_t burst = 0U;
    TickType_t next_diag = xTaskGetTickCount() + pdMS_TO_TICKS(GW_TCP_DIAG_PERIOD_MS);
    for (;;) {
        if ((int32_t)(xTaskGetTickCount() - next_diag) >= 0) {
            log_stats();
            next_diag = xTaskGetTickCount() + pdMS_TO_TICKS(GW_TCP_DIAG_PERIOD_MS);
        }
        if (!network_ready()) {
            GW_LOGW("TCP", "network not ready; closing client");
            break;
        }

        struct netbuf *buf = NULL;
        err_t err = netconn_recv(client, &buf);
        if (err == ERR_TIMEOUT) {
            continue;
        }
        if (err != ERR_OK) {
            if ((err != ERR_CLSD) && (err != ERR_RST) && (err != ERR_ABRT)) {
                taskENTER_CRITICAL();
                ++s_stats.recv_error_count;
                taskEXIT_CRITICAL();
                GW_LOGW("TCP", "receive ended err=%d", (int)err);
            }
            if (buf != NULL) {
                netbuf_delete(buf);
            }
            break;
        }
        if (buf == NULL) {
            break;
        }

        bool ok = echo_netbuf(client, buf);
        netbuf_delete(buf);
        if (!ok) {
            GW_LOGW("TCP", "send/receive processing failed; closing client");
            break;
        }

        ++burst;
        if (burst >= TCP_SERVER_RX_BURST_LIMIT) {
            burst = 0U;
            taskYIELD();
        }
    }

    taskENTER_CRITICAL();
    ++s_stats.disconnect_count;
    taskEXIT_CRITICAL();
    GW_LOGI("TCP", "client disconnected");
}

static struct netconn *create_listener(void)
{
    struct netconn *listener = netconn_new(NETCONN_TCP);
    if (listener == NULL) {
        return NULL;
    }

    netconn_set_recvtimeout(listener, (int)GW_TCP_ACCEPT_POLL_MS);
    netconn_set_sendtimeout(listener, (int)GW_TCP_SEND_TIMEOUT_MS);

    err_t err = netconn_bind(listener, IP_ADDR_ANY, (u16_t)GW_TCP_SERVER_PORT);
    if (err != ERR_OK) {
        GW_LOGE("TCP", "bind port %u failed err=%d",
                (unsigned)GW_TCP_SERVER_PORT, (int)err);
        (void)netconn_delete(listener);
        return NULL;
    }

    err = netconn_listen_with_backlog(listener, (u8_t)GW_TCP_SERVER_BACKLOG);
    if (err != ERR_OK) {
        GW_LOGE("TCP", "listen port %u failed err=%d",
                (unsigned)GW_TCP_SERVER_PORT, (int)err);
        (void)netconn_delete(listener);
        return NULL;
    }

    taskENTER_CRITICAL();
    s_stats.listening = true;
    ++s_stats.listener_start_count;
    taskEXIT_CRITICAL();
    (void)xEventGroupSetBits(g_system_events, EVT_TCP_SERVER_READY);

#if (GW_TCP_ECHO_ENABLE != 0U)
    GW_LOGI("TCP", "server listening on port %u (echo enabled)",
            (unsigned)GW_TCP_SERVER_PORT);
#else
    GW_LOGI("TCP", "server listening on port %u", (unsigned)GW_TCP_SERVER_PORT);
#endif
    return listener;
}

static void log_stats(void)
{
#if (GW_TCP_DIAGNOSTIC_LOG != 0U)
    gw_tcp_server_stats_t stats;
    gw_tcp_server_get_stats(&stats);
    GW_LOGI("TCP", "listen=%u client=%u acc=%lu disc=%lu rx=%lu/%luB tx=%lu/%luB rxErr=%lu txErr=%lu",
            stats.listening ? 1U : 0U,
            stats.client_connected ? 1U : 0U,
            (unsigned long)stats.accept_count,
            (unsigned long)stats.disconnect_count,
            (unsigned long)stats.recv_count,
            (unsigned long)stats.rx_bytes,
            (unsigned long)stats.send_count,
            (unsigned long)stats.tx_bytes,
            (unsigned long)stats.recv_error_count,
            (unsigned long)stats.send_error_count);
#endif
}

static void tcp_server_task(void *argument)
{
    (void)argument;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.task_started = true;
    s_stats.listen_port = (uint16_t)GW_TCP_SERVER_PORT;

    struct netconn *listener = NULL;
    TickType_t next_diag = xTaskGetTickCount() + pdMS_TO_TICKS(GW_TCP_DIAG_PERIOD_MS);

    for (;;) {
        (void)xEventGroupWaitBits(g_system_events, EVT_NET_IP_READY,
                                  pdFALSE, pdTRUE, portMAX_DELAY);

        if (listener == NULL) {
            listener = create_listener();
            if (listener == NULL) {
                taskENTER_CRITICAL();
                ++s_stats.accept_error_count;
                taskEXIT_CRITICAL();
                vTaskDelay(pdMS_TO_TICKS(GW_TCP_RETRY_MS));
                continue;
            }
        }

        while (network_ready()) {
            struct netconn *client = NULL;
            err_t err = netconn_accept(listener, &client);
            if (err == ERR_TIMEOUT) {
                if ((int32_t)(xTaskGetTickCount() - next_diag) >= 0) {
                    log_stats();
                    next_diag = xTaskGetTickCount() + pdMS_TO_TICKS(GW_TCP_DIAG_PERIOD_MS);
                }
                continue;
            }
            if (err != ERR_OK) {
                taskENTER_CRITICAL();
                ++s_stats.accept_error_count;
                taskEXIT_CRITICAL();
                GW_LOGW("TCP", "accept failed err=%d; listener restarting", (int)err);
                break;
            }
            if (client == NULL) {
                taskENTER_CRITICAL();
                ++s_stats.accept_error_count;
                taskEXIT_CRITICAL();
                continue;
            }

            serve_client(client);
            close_client(&client);
            next_diag = xTaskGetTickCount() + pdMS_TO_TICKS(GW_TCP_DIAG_PERIOD_MS);
        }

        delete_listener(&listener);
        taskENTER_CRITICAL();
        ++s_stats.listener_restart_count;
        taskEXIT_CRITICAL();

        if (!network_ready()) {
            GW_LOGI("TCP", "server paused; waiting for IP ready");
        } else {
            vTaskDelay(pdMS_TO_TICKS(GW_TCP_RETRY_MS));
        }
    }
}

void gw_tcp_server_task_create(void)
{
    TaskHandle_t handle = xTaskCreateStatic(tcp_server_task, "tcp_srv",
                                            TCP_SERVER_TASK_STACK_WORDS, NULL,
                                            TCP_SERVER_TASK_PRIORITY,
                                            s_tcp_server_stack,
                                            &s_tcp_server_tcb);
    configASSERT(handle != NULL);
}

void gw_tcp_server_get_stats(gw_tcp_server_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}

#else

void gw_tcp_server_task_create(void)
{
}

void gw_tcp_server_get_stats(gw_tcp_server_stats_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

#endif
