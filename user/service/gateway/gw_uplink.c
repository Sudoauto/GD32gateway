#include "gw_uplink.h"

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "task.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "device_manager.h"
#include "gw_command_router.h"
#include "gw_config.h"
#include "gw_diagnostics.h"
#include "gw_flash_store.h"
#include "gw_security.h"
#include "gw_ota.h"
#include "gw_watchdog.h"
#include "gateway_build_config.h"
#include "gw_log.h"
#include "gw_time.h"
#include "gw_types.h"
#include "point_db.h"
#include "rtos_objects.h"

#if ((GW_ETH_ENABLE != 0U) && (GW_UPLINK_ENABLE != 0U))

#define UPLINK_TASK_STACK_WORDS      1792U
#define UPLINK_TASK_PRIORITY         2U
#define UPLINK_FRAME_QUEUE_LEN       12U
#define UPLINK_POINT_BATCH           8U
#define UPLINK_LOOP_MS               50U
#define UPLINK_ACCEPT_MS             500U
#define UPLINK_SEND_TIMEOUT_MS       1000U
#define UPLINK_JSON_LINE_MAX         896U
#define UPLINK_ALERT_QUEUE_LEN       8U

#define UPLINK_FLAG_CAN_EXT          (1U << 0)
#define UPLINK_FLAG_CAN_FD           (1U << 1)
#define UPLINK_FLAG_CAN_BRS          (1U << 2)
#define UPLINK_FLAG_CAN_ESI          (1U << 3)

typedef gw_uplink_event_t gw_uplink_frame_t;

typedef struct {
    uint16_t length;
    char line[384];
} gw_uplink_alert_t;

static StaticTask_t s_task_cb;
static StackType_t s_stack[UPLINK_TASK_STACK_WORDS];
static StaticQueue_t s_frame_q_cb;
static uint8_t s_frame_q_storage[UPLINK_FRAME_QUEUE_LEN * sizeof(gw_uplink_frame_t)];
static QueueHandle_t s_frame_q;
static StaticQueue_t s_alert_q_cb;
static uint8_t s_alert_q_storage[UPLINK_ALERT_QUEUE_LEN * sizeof(gw_uplink_alert_t)];
static QueueHandle_t s_alert_q;
static gw_uplink_stats_t s_stats;
static gw_uplink_event_t s_history[GW_UPLINK_HISTORY_DEPTH];
static uint32_t s_history_head;
static uint32_t s_history_count;
static uint32_t s_event_sequence;
static char s_config_export[GW_CONFIG_MAX_CSV_BYTES + 1U];

static uint64_t record_time_ms(void)
{
    uint64_t utc=gw_time_utc_ms();
    return (utc!=0U)?utc:gw_time_ms();
}

static const char *if_text(gw_interface_id_t id)
{
    switch (id) {
    case GW_IF_RS485_0: return "rs485_0";
    case GW_IF_CANFD_0: return "canfd_0";
    case GW_IF_ETH_0: return "eth_0";
    default: return "unknown";
    }
}

static const char *proto_text(gw_protocol_t p)
{
    switch (p) {
    case GW_PROTO_MODBUS_RTU: return "modbus_rtu";
    case GW_PROTO_MODBUS_TCP: return "modbus_tcp";
    case GW_PROTO_CAN: return "canfd";
    case GW_PROTO_RS485_RAW: return "rs485_raw";
    default: return "none";
    }
}

static const char *quality_text(gw_quality_t q)
{
    switch (q) {
    case GW_QUALITY_GOOD: return "good";
    case GW_QUALITY_STALE: return "stale";
    case GW_QUALITY_TIMEOUT: return "timeout";
    case GW_QUALITY_BAD: return "bad";
    case GW_QUALITY_OFFLINE: return "offline";
    case GW_QUALITY_INVALID: return "invalid";
    default: return "unknown";
    }
}

static const char *value_type_text(gw_value_type_t type)
{
    switch (type) {
    case GW_VALUE_BOOL: return "bool";
    case GW_VALUE_U16: return "u16";
    case GW_VALUE_I16: return "i16";
    case GW_VALUE_U32: return "u32";
    case GW_VALUE_I32: return "i32";
    case GW_VALUE_F32: return "f32";
    case GW_VALUE_F64: return "f64";
    default: return "unknown";
    }
}

static void stats_set_listening(bool value)
{
    taskENTER_CRITICAL();
    s_stats.listening = value;
    taskEXIT_CRITICAL();
}

static void stats_set_client(bool value)
{
    taskENTER_CRITICAL();
    s_stats.client_connected = value;
    taskEXIT_CRITICAL();
}

static bool network_ready(void)
{
    if (g_system_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(g_system_events) & EVT_NET_IP_READY) != 0U;
}

static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0U;
    if ((dst == NULL) || (dst_size == 0U)) {
        return 0U;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0U;
    }
    while ((*src != '\0') && (out + 1U < dst_size)) {
        char c = *src++;
        if ((c == '"') || (c == '\\')) {
            if (out + 2U >= dst_size) break;
            dst[out++] = '\\';
            dst[out++] = c;
        } else if (((unsigned char)c >= 0x20U) && ((unsigned char)c < 0x7FU)) {
            dst[out++] = c;
        } else {
            dst[out++] = '?';
        }
    }
    dst[out] = '\0';
    return out;
}

static void fixed3_text(double value, char *out, size_t out_size)
{
    if ((out == NULL) || (out_size == 0U)) {
        return;
    }
    if ((value != value) || (value > 9000000000000.0) ||
        (value < -9000000000000.0)) {
        (void)snprintf(out, out_size, "null");
        return;
    }
    bool negative = value < 0.0;
    double abs_value = negative ? -value : value;
    uint64_t scaled = (uint64_t)(abs_value * 1000.0 + 0.5);
    uint64_t whole = scaled / 1000ULL;
    uint32_t frac = (uint32_t)(scaled % 1000ULL);
    (void)snprintf(out, out_size, "%s%llu.%03lu",
                   negative ? "-" : "",
                   (unsigned long long)whole,
                   (unsigned long)frac);
}

static void point_value_json(const gw_point_t *point, char *out, size_t out_size)
{
    if ((point == NULL) || (out == NULL) || (out_size == 0U)) {
        return;
    }
    switch (point->type) {
    case GW_VALUE_BOOL:
        (void)snprintf(out, out_size, "%s", point->value.b ? "true" : "false");
        break;
    case GW_VALUE_U16:
        (void)snprintf(out, out_size, "%u", (unsigned)point->value.u16);
        break;
    case GW_VALUE_I16:
        (void)snprintf(out, out_size, "%d", (int)point->value.i16);
        break;
    case GW_VALUE_U32:
        (void)snprintf(out, out_size, "%lu", (unsigned long)point->value.u32);
        break;
    case GW_VALUE_I32:
        (void)snprintf(out, out_size, "%ld", (long)point->value.i32);
        break;
    case GW_VALUE_F32:
        fixed3_text((double)point->value.f32, out, out_size);
        break;
    case GW_VALUE_F64:
        fixed3_text(point->value.f64, out, out_size);
        break;
    default:
        (void)snprintf(out, out_size, "null");
        break;
    }
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
        if ((err != ERR_OK) || (written == 0U)) {
            taskENTER_CRITICAL();
            ++s_stats.send_error_count;
            taskEXIT_CRITICAL();
            return (err != ERR_OK) ? err : ERR_IF;
        }
        offset += written;
        taskENTER_CRITICAL();
        s_stats.tx_bytes += (uint32_t)written;
        taskEXIT_CRITICAL();
    }
    return ERR_OK;
}

static bool send_line(struct netconn *client, const char *line)
{
    if ((client == NULL) || (line == NULL)) {
        return false;
    }
    return send_all(client, line, strlen(line)) == ERR_OK;
}

static int hex_nibble(char c)
{
    if ((c >= '0') && (c <= '9')) return c - '0';
    if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
    if ((c >= 'A') && (c <= 'F')) return c - 'A' + 10;
    return -1;
}

static char *next_token(char **cursor)
{
    if ((cursor == NULL) || (*cursor == NULL) || (**cursor == '\0')) return NULL;
    char *start = *cursor;
    char *p = start;
    while ((*p != '\0') && (*p != ',')) ++p;
    if (*p == ',') { *p = '\0'; *cursor = p + 1; }
    else { *cursor = p; }
    return start;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if ((s == NULL) || (out == NULL) || (*s == '\0')) return false;
    uint32_t base = 10U;
    if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) { base = 16U; s += 2; }
    if (*s == '\0') return false;
    uint32_t v = 0U;
    while (*s != '\0') {
        int d = hex_nibble(*s++);
        if ((d < 0) || ((uint32_t)d >= base)) return false;
        if (v > ((UINT32_MAX - (uint32_t)d) / base)) return false;
        v = v * base + (uint32_t)d;
    }
    *out = v;
    return true;
}

static bool parse_hex(const char *s, uint8_t *out, uint8_t len)
{
    if ((s == NULL) || (out == NULL)) return false;
    for (uint8_t i = 0U; i < len; ++i) {
        int hi = hex_nibble(s[i * 2U]);
        int lo = hex_nibble(s[i * 2U + 1U]);
        if ((hi < 0) || (lo < 0)) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return s[(size_t)len * 2U] == '\0';
}

static bool parse_hex_variable(const char *s, uint8_t *out, uint16_t cap, uint16_t *length)
{
    if ((s == NULL) || (out == NULL) || (length == NULL)) return false;
    size_t chars = strlen(s);
    if ((chars == 0U) || ((chars & 1U) != 0U) || ((chars / 2U) > cap)) return false;
    uint16_t n = (uint16_t)(chars / 2U);
    for (uint16_t i = 0U; i < n; ++i) {
        int hi = hex_nibble(s[(size_t)i * 2U]);
        int lo = hex_nibble(s[(size_t)i * 2U + 1U]);
        if ((hi < 0) || (lo < 0)) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *length = n;
    return true;
}

static bool send_ack(struct netconn *client, const char *cmd, gw_err_t result)
{
    char line[144];
    int n = snprintf(line, sizeof(line),
                     "{\"v\":2,\"kind\":\"ack\",\"cmd\":\"%s\",\"ok\":%u,\"err\":%ld}\n",
                     cmd != NULL ? cmd : "?", result == GW_OK ? 1U : 0U,
                     (long)result);
    return (n > 0) && ((size_t)n < sizeof(line)) && send_line(client, line);
}

static gw_err_t send_config_dump(struct netconn *client)
{
    size_t n = gw_config_export_csv(s_config_export, sizeof(s_config_export));
    if ((n == 0U) || (n >= sizeof(s_config_export))) return GW_ERR_IO;
    s_config_export[n] = '\0';
    char *row = s_config_export;
    while (*row != '\0') {
        char *end = strchr(row, '\n');
        if (end != NULL) *end = '\0';
        if ((*row != '\0') && (*row != '#')) {
            char escaped[720]; char out[800];
            (void)json_escape(row, escaped, sizeof(escaped));
            int m = snprintf(out, sizeof(out),
                             "{\"v\":2,\"kind\":\"config\",\"row\":\"%s\"}\n",
                             escaped);
            if ((m <= 0) || ((size_t)m >= sizeof(out)) || !send_line(client, out)) return GW_ERR_IO;
        }
        if (end == NULL) break;
        row = end + 1;
    }
    return GW_OK;
}

static gw_err_t process_command(struct netconn *client, char *line,
                                bool *authenticated)
{
    char *cursor = line;
    char *cmd = next_token(&cursor);
    if ((cmd == NULL) || (authenticated == NULL)) return GW_ERR_PARAM;
    gw_err_t result = GW_ERR_PARAM;

    if (strcmp(cmd, "PING") == 0) {
        result = GW_OK;
    } else if (strcmp(cmd, "AUTH") == 0) {
        char *user = next_token(&cursor);
        char *password = next_token(&cursor);
        if ((user != NULL) && (password != NULL) &&
            gw_security_authenticate(user, password)) {
            *authenticated = true;
            if (g_system_events != NULL)
                (void)xEventGroupSetBits(g_system_events, EVT_UPLINK_CLIENT_CONNECTED);
            result = GW_OK;
        } else {
            taskENTER_CRITICAL(); ++s_stats.auth_fail; taskEXIT_CRITICAL();
            result = GW_ERR_AUTH;
        }
    } else if ((GW_AUTH_ENABLE != 0U) && !*authenticated) {
        result = GW_ERR_AUTH;
    } else if (strcmp(cmd, "CAN") == 0) {
        char *id_s = next_token(&cursor);
        char *len_s = next_token(&cursor);
        char *hex_s = next_token(&cursor);
        uint32_t id = 0U, len32 = 0U;
        uint8_t data[CANFD_MAX_DATA_BYTES];
        memset(data, 0, sizeof(data));
        if (parse_u32(id_s, &id) && parse_u32(len_s, &len32) &&
            (len32 <= CANFD_MAX_DATA_BYTES) &&
            parse_hex(hex_s, data, (uint8_t)len32)) {
            result = gw_command_send_can(id, id > 0x7FFU, true,
                                         data, (uint8_t)len32);
        }
    } else if (strcmp(cmd, "MBR") == 0) {
        uint32_t slave = 0U, reg = 0U, qty = 0U;
        if (parse_u32(next_token(&cursor), &slave) &&
            parse_u32(next_token(&cursor), &reg) &&
            parse_u32(next_token(&cursor), &qty) &&
            (slave >= 1U) && (slave <= 247U) &&
            (reg <= 0xFFFFU) && (qty >= 1U) && (qty <= 125U)) {
            result = gw_command_modbus_read_holding_slave((uint8_t)slave,
                                                           (uint16_t)reg,
                                                           (uint16_t)qty);
        }
    } else if (strcmp(cmd, "MBW") == 0) {
        uint32_t slave = 0U, reg = 0U, value = 0U;
        if (parse_u32(next_token(&cursor), &slave) &&
            parse_u32(next_token(&cursor), &reg) &&
            parse_u32(next_token(&cursor), &value) &&
            (slave >= 1U) && (slave <= 247U) &&
            (reg <= 0xFFFFU) && (value <= 0xFFFFU)) {
            result = gw_command_modbus_write_single_slave((uint8_t)slave,
                                                           (uint16_t)reg,
                                                           (uint16_t)value);
        }
    } else if (strcmp(cmd, "MBRD") == 0) {
        uint32_t dev = 0U, reg = 0U, qty = 0U;
        if (parse_u32(next_token(&cursor), &dev) &&
            parse_u32(next_token(&cursor), &reg) &&
            parse_u32(next_token(&cursor), &qty) &&
            (dev != 0U) && (reg <= 0xFFFFU) &&
            (qty >= 1U) && (qty <= 125U)) {
            result = gw_command_modbus_read_holding(dev, (uint16_t)reg,
                                                     (uint16_t)qty);
        }
    } else if (strcmp(cmd, "MBWD") == 0) {
        uint32_t dev = 0U, reg = 0U, value = 0U;
        if (parse_u32(next_token(&cursor), &dev) &&
            parse_u32(next_token(&cursor), &reg) &&
            parse_u32(next_token(&cursor), &value) &&
            (dev != 0U) && (reg <= 0xFFFFU) && (value <= 0xFFFFU)) {
            result = gw_command_modbus_write_single(dev, (uint16_t)reg,
                                                     (uint16_t)value);
        }
    } else if (strcmp(cmd, "CFGSET") == 0) {
        /* Cursor intentionally keeps every comma after CFGSET; it is the full
         * CSV object row (POINT/CANMAP/POLL/ALARM/RULE/NET/RS485/...). */
        result = ((cursor != NULL) && (*cursor != '\0')) ?
                 gw_config_apply_csv_line(cursor, true) : GW_ERR_PARAM;
    } else if (strcmp(cmd, "CFGDEL") == 0) {
        char *kind = next_token(&cursor);
        uint32_t id = 0U;
        if ((kind != NULL) && parse_u32(next_token(&cursor), &id)) {
            result = gw_config_delete(kind, id, true);
        }
    } else if (strcmp(cmd, "CFGGET") == 0) {
        result = send_config_dump(client);
    } else if (strcmp(cmd, "CFGSAVE") == 0) {
        gw_config_request_save();
        result = GW_OK;
    } else if (strcmp(cmd, "PASS") == 0) {
        char *user = next_token(&cursor);
        char *password = next_token(&cursor);
        result = gw_security_set_password(user, password);
        if (result == GW_OK) {
            gw_config_request_save();
        }
    } else if (strcmp(cmd, "OTA_BEGIN") == 0) {
        char *ver_s=next_token(&cursor),*size_s=next_token(&cursor),*sha_s=next_token(&cursor),*sig_s=next_token(&cursor),*enc_s=next_token(&cursor);
        uint32_t ver=0U,size=0U,enc=0U;gw_ota_manifest_t m;memset(&m,0,sizeof(m));uint16_t sig_len=0U;
        if(parse_u32(ver_s,&ver)&&parse_u32(size_s,&size)&&parse_u32(enc_s,&enc)&&
           (size!=0U)&&(enc<=1U)&&parse_hex(sha_s,m.image_sha256,32U)&&
           parse_hex_variable(sig_s,m.signature,GW_OTA_SIGNATURE_MAX,&sig_len)){
            m.version=ver;m.image_size=size;m.signature_length=sig_len;m.encrypted=(enc!=0U);result=gw_ota_begin(&m);
        }
    } else if (strcmp(cmd, "OTA_DATA") == 0) {
        char *off_s=next_token(&cursor),*data_s=next_token(&cursor);uint32_t off=0U;uint8_t data[256];uint16_t data_len=0U;
        if(parse_u32(off_s,&off)&&parse_hex_variable(data_s,data,sizeof(data),&data_len)&&data_len!=0U)result=gw_ota_write(off,data,data_len);
    } else if (strcmp(cmd, "OTA_END") == 0) {
        result=gw_ota_finalize();
    } else if (strcmp(cmd, "OTA_ABORT") == 0) {
        gw_ota_abort();result=GW_OK;
    } else if (strcmp(cmd, "OTA_STATUS") == 0) {
        gw_ota_status_t st;gw_ota_get_status(&st);char msg[160];int n=snprintf(msg,sizeof(msg),"{\"v\":2,\"kind\":\"ota\",\"state\":%u,\"expected\":%lu,\"received\":%lu,\"err\":%ld}\n",(unsigned)st.state,(unsigned long)st.expected_size,(unsigned long)st.received,(long)st.last_error);
        result=((n>0)&&((size_t)n<sizeof(msg))&&send_line(client,msg))?GW_OK:GW_ERR_IO;
    } else if (strcmp(cmd, "FACTORY") == 0) {
        char *confirm = next_token(&cursor);
        if ((confirm != NULL) && (strcmp(confirm, "YES") == 0)) {
            gw_config_request_factory_reset(true);
            result = GW_OK;
        }
    } else if (strcmp(cmd, "SELFTEST") == 0) {
        gw_diagnostics_run_selftest();
        result = GW_OK;
    }

    taskENTER_CRITICAL();
    ++s_stats.rx_command_count;
    if (result != GW_OK) ++s_stats.command_error_count;
    taskEXIT_CRITICAL();
    (void)send_ack(client, cmd, result);
    return result;
}

static bool consume_command_bytes(struct netconn *client, const uint8_t *data,
                                  uint16_t len, char *line, size_t *line_len,
                                  size_t line_size, bool *authenticated)
{
    for (uint16_t i = 0U; i < len; ++i) {
        char c = (char)data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            line[*line_len] = '\0';
            if (*line_len != 0U) (void)process_command(client, line, authenticated);
            *line_len = 0U;
        } else if ((*line_len + 1U) < line_size) {
            line[(*line_len)++] = c;
        } else {
            *line_len = 0U;
            taskENTER_CRITICAL(); ++s_stats.command_error_count; taskEXIT_CRITICAL();
            return false;
        }
    }
    return true;
}

static bool consume_command_netbuf(struct netconn *client, struct netbuf *buf,
                                   char *line, size_t *line_len, size_t line_size,
                                   bool *authenticated)
{
    if ((client == NULL) || (buf == NULL)) return false;
    netbuf_first(buf);
    for (;;) {
        void *data = NULL;
        u16_t len = 0U;
        if (netbuf_data(buf, &data, &len) != ERR_OK) return false;
        if ((len != 0U) && !consume_command_bytes(client, (const uint8_t *)data,
                                                  len, line, line_len, line_size, authenticated)) {
            return false;
        }
        if (netbuf_next(buf) < 0) break;
    }
    return true;
}

static bool send_hello(struct netconn *client)
{
    static const char hello[] =
        "{\"v\":2,\"kind\":\"hello\",\"model\":\"GD32H759-gateway\","
        "\"fw\":\"0.9.0\",\"format\":\"gw-jsonl-2\","
        "\"auth_required\":"
#if (GW_AUTH_ENABLE != 0U)
        "true"
#else
        "false"
#endif
        "}\n";
    return send_line(client, hello);
}

static int format_point_json(const gw_point_t *point, bool snapshot,
                             bool history, char *line, size_t line_size)
{
    if ((point==NULL)||(line==NULL)||(line_size==0U)) return -1;
    gw_device_t device;memset(&device,0,sizeof(device));
    if(device_manager_get(point->device_id,&device)!=GW_OK){device.interface_id=GW_IF_ETH_0;device.protocol=GW_PROTO_NONE;}
    char name[GW_POINT_NAME_LEN*2U+4U],value[48];memset(name,0,sizeof(name));memset(value,0,sizeof(value));json_escape(point->name,name,sizeof(name));point_value_json(point,value,sizeof(value));
    uint64_t ts=point->timestamp_ms; if(ts==0U)ts=record_time_ms();
    return snprintf(line,line_size,
        "{\"v\":2,\"kind\":\"point\",\"mode\":\"%s\",\"history\":%s,"
        "\"if\":\"%s\",\"proto\":\"%s\",\"device\":%lu,"
        "\"point\":%lu,\"name\":\"%s\",\"ts\":%llu,\"utc\":%s,"
        "\"quality\":\"%s\",\"type\":\"%s\",\"value\":%s,\"rev\":%lu}\n",
        snapshot?"snapshot":"delta",history?"true":"false",if_text(device.interface_id),proto_text(device.protocol),
        (unsigned long)point->device_id,(unsigned long)point->id,name,(unsigned long long)ts,
        gw_time_is_synchronized()?"true":"false",quality_text(point->quality),value_type_text(point->type),value,(unsigned long)point->revision);
}

static bool send_point(struct netconn *client,const gw_point_t *point,bool snapshot)
{
    char line[UPLINK_JSON_LINE_MAX];int n=format_point_json(point,snapshot,false,line,sizeof(line));
    if((n<=0)||((size_t)n>=sizeof(line))||!send_line(client,line))return false;
    taskENTER_CRITICAL();++s_stats.point_sent;if(snapshot)++s_stats.snapshot_sent;taskEXIT_CRITICAL();return true;
}

static int format_frame_json(const gw_uplink_frame_t *frame,bool history,char *line,size_t line_size)
{
    if((frame==NULL)||(line==NULL))return -1;char hex[GW_UPLINK_RAW_MAX*2U+1U];static const char d[]="0123456789ABCDEF";uint16_t len=frame->length;if(len>GW_UPLINK_RAW_MAX)len=GW_UPLINK_RAW_MAX;for(uint16_t i=0U;i<len;++i){hex[i*2U]=d[(frame->data[i]>>4U)&15U];hex[i*2U+1U]=d[frame->data[i]&15U];}hex[len*2U]='\0';
    return snprintf(line,line_size,
      "{\"v\":2,\"kind\":\"frame\",\"history\":%s,\"seq\":%lu,\"dir\":\"%s\","
      "\"if\":\"%s\",\"proto\":\"%s\",\"device\":%lu,\"ts\":%llu,\"utc\":%s,"
      "\"addr\":%lu,\"code\":%u,\"flags\":%u,\"result\":%ld,\"len\":%u,\"data\":\"%s\"}\n",
      history?"true":"false",(unsigned long)frame->sequence,frame->tx_direction?"tx":"rx",if_text(frame->interface_id),proto_text(frame->protocol),
      (unsigned long)frame->device_id,(unsigned long long)frame->timestamp_ms,gw_time_is_synchronized()?"true":"false",(unsigned long)frame->address,(unsigned)frame->code,(unsigned)frame->flags,(long)frame->result,(unsigned)len,hex);
}
static bool send_frame(struct netconn *client,const gw_uplink_frame_t *frame)
{
    char line[UPLINK_JSON_LINE_MAX];int n=format_frame_json(frame,false,line,sizeof(line));if((n<=0)||((size_t)n>=sizeof(line))||!send_line(client,line))return false;taskENTER_CRITICAL();++s_stats.frame_sent;taskEXIT_CRITICAL();return true;
}

static void history_push(gw_uplink_frame_t *frame)
{
    taskENTER_CRITICAL();
    ++s_event_sequence;
    if (s_event_sequence == 0U) ++s_event_sequence;
    frame->sequence = s_event_sequence;
    s_history[s_history_head] = *frame;
    s_history_head = (s_history_head + 1U) % GW_UPLINK_HISTORY_DEPTH;
    if (s_history_count < GW_UPLINK_HISTORY_DEPTH) ++s_history_count;
    s_stats.history_count = s_history_count;
    taskEXIT_CRITICAL();
}

static void enqueue_frame(gw_uplink_frame_t *frame)
{
    if ((frame == NULL) || (s_frame_q == NULL)) return;
    history_push(frame);

    if ((g_system_events == NULL) ||
        ((xEventGroupGetBits(g_system_events) & EVT_UPLINK_CLIENT_CONNECTED) == 0U)) {
#if (GW_OFFLINE_SPOOL_ENABLE != 0U)
        char line[UPLINK_JSON_LINE_MAX];int n=format_frame_json(frame,true,line,sizeof(line));
        if((n>0)&&((size_t)n<GW_SPOOL_PAYLOAD_MAX)&&
           (gw_flash_spool_append(line,(uint16_t)n,frame->timestamp_ms,NULL)==GW_OK)){taskENTER_CRITICAL();++s_stats.offline_spooled;taskEXIT_CRITICAL();}
#endif
        return;
    }

    if (xQueueSend(s_frame_q, frame, 0U) == pdTRUE) {
        taskENTER_CRITICAL(); ++s_stats.frame_queued; taskEXIT_CRITICAL();
    } else {
        taskENTER_CRITICAL(); ++s_stats.frame_dropped; taskEXIT_CRITICAL();
    }
}

void gw_uplink_publish_event(const gw_uplink_event_t *event)
{
    if (event == NULL) return;
    gw_uplink_frame_t copy = *event;
    if (copy.length > GW_UPLINK_RAW_MAX) copy.length = GW_UPLINK_RAW_MAX;
    if (copy.timestamp_ms == 0U) copy.timestamp_ms = record_time_ms();
    enqueue_frame(&copy);
}

void gw_uplink_publish_can(const canfd_frame_t *frame, bool tx_direction)
{
    if (frame == NULL) {
        return;
    }
    gw_uplink_frame_t event;
    memset(&event, 0, sizeof(event));
    event.interface_id = GW_IF_CANFD_0;
    event.protocol = GW_PROTO_CAN;
    event.tx_direction = tx_direction;
    event.timestamp_ms = record_time_ms();
    event.address = frame->id;
    event.flags = (frame->extended ? UPLINK_FLAG_CAN_EXT : 0U) |
                  (frame->fd ? UPLINK_FLAG_CAN_FD : 0U) |
                  (frame->brs ? UPLINK_FLAG_CAN_BRS : 0U) |
                  (frame->esi ? UPLINK_FLAG_CAN_ESI : 0U);
    event.length = frame->len;
    if (event.length > CANFD_MAX_DATA_BYTES) event.length = CANFD_MAX_DATA_BYTES;
    if (event.length != 0U) memcpy(event.data, frame->data, event.length);
    event.result = GW_OK;
    gw_uplink_publish_event(&event);
}

void gw_uplink_publish_modbus(const uint8_t *frame, uint16_t length,
                              uint32_t device_id, uint8_t slave,
                              bool tx_direction, gw_err_t result)
{
    if ((frame == NULL) && (length != 0U)) {
        return;
    }
    gw_uplink_frame_t event;
    memset(&event, 0, sizeof(event));
    event.interface_id = GW_IF_RS485_0;
    event.protocol = GW_PROTO_MODBUS_RTU;
    event.tx_direction = tx_direction;
    event.device_id = device_id;
    event.timestamp_ms = record_time_ms();
    event.address = slave;
    event.code = ((frame != NULL) && (length >= 2U)) ? frame[1] : 0U;
    event.result = result;
    event.length = (length > GW_UPLINK_RAW_MAX) ? GW_UPLINK_RAW_MAX : length;
    if (event.length != 0U) memcpy(event.data, frame, event.length);
    gw_uplink_publish_event(&event);
}

void gw_uplink_publish_point_record(uint32_t point_id)
{
    gw_point_t p;if(point_db_get(point_id,&p)!=GW_OK)return;
    if((g_system_events!=NULL)&&((xEventGroupGetBits(g_system_events)&EVT_UPLINK_CLIENT_CONNECTED)!=0U))return;
#if (GW_OFFLINE_SPOOL_ENABLE!=0U)
    char line[UPLINK_JSON_LINE_MAX];int n=format_point_json(&p,false,true,line,sizeof(line));if((n>0)&&((size_t)n<GW_SPOOL_PAYLOAD_MAX)&&gw_flash_spool_append(line,(uint16_t)n,p.timestamp_ms,NULL)==GW_OK){taskENTER_CRITICAL();++s_stats.offline_spooled;taskEXIT_CRITICAL();}
#endif
}

static void enqueue_alert_line(const char *line,uint16_t length)
{
    if((line==NULL)||(length==0U))return;
    if((g_system_events==NULL)||((xEventGroupGetBits(g_system_events)&EVT_UPLINK_CLIENT_CONNECTED)==0U)){
#if (GW_OFFLINE_SPOOL_ENABLE!=0U)
        char stored[384]; uint16_t stored_len=length;
        if(stored_len>=sizeof(stored)) stored_len=(uint16_t)(sizeof(stored)-1U);
        memcpy(stored,line,stored_len); stored[stored_len]='\0';
        char *h=strstr(stored,"\"history\":false");
        if(h!=NULL){ /* false -> true plus one-byte shrink by shifting tail left. */
            size_t off=(size_t)(h-stored)+10U; /* points to 'f' in false */
            memmove(&stored[off+4U],&stored[off+5U],strlen(&stored[off+5U])+1U);
            memcpy(&stored[off],"true",4U); --stored_len;
        }
        if(stored_len<GW_SPOOL_PAYLOAD_MAX&&gw_flash_spool_append(stored,stored_len,record_time_ms(),NULL)==GW_OK){taskENTER_CRITICAL();++s_stats.offline_spooled;taskEXIT_CRITICAL();}
#endif
        return;
    }
    gw_uplink_alert_t a;memset(&a,0,sizeof(a));a.length=(length<sizeof(a.line))?length:(uint16_t)(sizeof(a.line)-1U);memcpy(a.line,line,a.length);if(xQueueSendToFront(s_alert_q,&a,0U)!=pdTRUE){taskENTER_CRITICAL();++s_stats.frame_dropped;taskEXIT_CRITICAL();}
}
void gw_uplink_publish_alarm(uint32_t alarm_id,uint32_t point_id,uint32_t kind,bool active,uint8_t priority,double value,double threshold)
{
    char line[384];int n=snprintf(line,sizeof(line),"{\"v\":2,\"kind\":\"alarm\",\"priority\":%u,\"state\":\"%s\",\"alarm\":%lu,\"point\":%lu,\"alarm_kind\":%lu,\"value\":%.6g,\"threshold\":%.6g,\"ts\":%llu,\"history\":false}\n",priority,active?"raised":"cleared",(unsigned long)alarm_id,(unsigned long)point_id,(unsigned long)kind,value,threshold,(unsigned long long)record_time_ms());if(n>0&&(size_t)n<sizeof(line))enqueue_alert_line(line,(uint16_t)n);
}
void gw_uplink_publish_rule_action(uint32_t rule_id,uint32_t point_id,gw_err_t result)
{
    char line[256];int n=snprintf(line,sizeof(line),"{\"v\":2,\"kind\":\"rule_action\",\"rule\":%lu,\"point\":%lu,\"ok\":%s,\"err\":%ld,\"ts\":%llu,\"history\":false}\n",(unsigned long)rule_id,(unsigned long)point_id,result==GW_OK?"true":"false",(long)result,(unsigned long long)record_time_ms());if(n>0&&(size_t)n<sizeof(line))enqueue_alert_line(line,(uint16_t)n);
}

static bool send_snapshot(struct netconn *client)
{
    gw_point_t points[UPLINK_POINT_BATCH];
    uint32_t offset = 0U;

    /* Page through Point DB without keeping the database mutex while doing
     * network I/O and without placing a GW_MAX_POINTS array on the task stack. */
    for (;;) {
        uint32_t count = point_db_snapshot_range(offset, points, UPLINK_POINT_BATCH);
        if (count == 0U) break;
        for (uint32_t i = 0U; i < count; ++i) {
            if (!send_point(client, &points[i], true)) return false;
            if (points[i].dirty && (points[i].revision != 0U)) {
                (void)point_db_ack_dirty(points[i].id, points[i].revision);
            }
        }
        offset += count;
        taskYIELD();
        if (count < UPLINK_POINT_BATCH) break;
    }
    return true;
}

static bool send_dirty_points(struct netconn *client)
{
    gw_point_t points[UPLINK_POINT_BATCH];
    uint32_t count = point_db_collect_dirty(points, UPLINK_POINT_BATCH, false);
    for (uint32_t i = 0U; i < count; ++i) {
        if (!send_point(client, &points[i], false)) return false;
        (void)point_db_ack_dirty(points[i].id, points[i].revision);
    }
    return true;
}

static bool drain_alerts(struct netconn *client)
{
    gw_uplink_alert_t a;while(xQueueReceive(s_alert_q,&a,0U)==pdTRUE){if(send_all(client,a.line,a.length)!=ERR_OK)return false;taskENTER_CRITICAL();++s_stats.alarm_sent;taskEXIT_CRITICAL();}return true;
}
static bool replay_offline(struct netconn *client)
{
#if (GW_OFFLINE_SPOOL_ENABLE!=0U)
    for(uint32_t i=0U;i<GW_OFFLINE_REPLAY_BURST;++i){gw_spool_record_t r;gw_err_t e=gw_flash_spool_peek(&r);if(e==GW_ERR_NOT_FOUND)break;if(e!=GW_OK)return true;if(send_all(client,r.payload,r.length)!=ERR_OK)return false;if(gw_flash_spool_pop(r.sequence)!=GW_OK)return false;taskENTER_CRITICAL();++s_stats.offline_replayed;taskEXIT_CRITICAL();}
#else
    (void)client;
#endif
    return true;
}

static bool drain_frames(struct netconn *client)
{
    gw_uplink_frame_t frame;
    uint32_t burst = 0U;
    while ((burst < 4U) && (xQueueReceive(s_frame_q, &frame, 0U) == pdTRUE)) {
        if (!send_frame(client, &frame)) return false;
        ++burst;
    }
    return true;
}

static struct netconn *create_listener(void)
{
    struct netconn *listener = netconn_new(NETCONN_TCP);
    if (listener == NULL) return NULL;
    netconn_set_recvtimeout(listener, (int)UPLINK_ACCEPT_MS);
    netconn_set_sendtimeout(listener, (int)UPLINK_SEND_TIMEOUT_MS);
    if (netconn_bind(listener, IP_ADDR_ANY, (u16_t)GW_UPLINK_PORT) != ERR_OK ||
        netconn_listen_with_backlog(listener, 1U) != ERR_OK) {
        (void)netconn_delete(listener);
        return NULL;
    }
    stats_set_listening(true);
    (void)xEventGroupSetBits(g_system_events, EVT_UPLINK_SERVER_READY);
    GW_LOGI("UPLINK", "GW-JSONL server listening on port %u", (unsigned)GW_UPLINK_PORT);
    return listener;
}

static void close_client(struct netconn **client)
{
    if ((client == NULL) || (*client == NULL)) return;
    (void)netconn_close(*client);
    (void)netconn_delete(*client);
    *client = NULL;
    stats_set_client(false);
    (void)xEventGroupClearBits(g_system_events, EVT_UPLINK_CLIENT_CONNECTED);
}

static void uplink_task(void *argument)
{
    (void)argument;
    s_stats.task_started = true;
    s_stats.listen_port = (uint16_t)GW_UPLINK_PORT;

    struct netconn *listener = NULL;
    for (;;) {
        while (!network_ready()) {
            gw_watchdog_beat(GW_WD_UPLINK);
            vTaskDelay(pdMS_TO_TICKS(250U));
        }
        if (listener == NULL) {
            listener = create_listener();
            if (listener == NULL) {
                gw_watchdog_beat(GW_WD_UPLINK);
                vTaskDelay(pdMS_TO_TICKS(1000U));
                continue;
            }
        }

        while (network_ready()) {
            gw_watchdog_beat(GW_WD_UPLINK);
            struct netconn *client = NULL;
            err_t err = netconn_accept(listener, &client);
            if (err == ERR_TIMEOUT) continue;
            if ((err != ERR_OK) || (client == NULL)) break;

            netconn_set_recvtimeout(client, (int)UPLINK_LOOP_MS);
            netconn_set_sendtimeout(client, (int)UPLINK_SEND_TIMEOUT_MS);
            stats_set_client(true);
            taskENTER_CRITICAL(); ++s_stats.accept_count; taskEXIT_CRITICAL();
            if (GW_AUTH_ENABLE == 0U)
                (void)xEventGroupSetBits(g_system_events, EVT_UPLINK_CLIENT_CONNECTED);
            GW_LOGI("UPLINK", "client connected; authentication %s",
                    (GW_AUTH_ENABLE != 0U) ? "required" : "disabled");
            char command_line[640];
            size_t command_len = 0U;
            memset(command_line, 0, sizeof(command_line));
            bool authenticated = (GW_AUTH_ENABLE == 0U);
            bool snapshot_sent = false;

            bool ok = send_hello(client);
            while (ok && network_ready()) {
                gw_watchdog_beat(GW_WD_UPLINK);
                if (authenticated) {
                    if (!snapshot_sent) {
                        ok = send_snapshot(client);
                        snapshot_sent = ok;
                    }
                    if (ok) ok = drain_alerts(client);
                    if (ok) ok = replay_offline(client);
                    if (ok) ok = drain_frames(client);
                    if (ok) ok = send_dirty_points(client);
                    if (!ok) break;
                }

                struct netbuf *buf = NULL;
                err = netconn_recv(client, &buf);
                if (err == ERR_TIMEOUT) continue;
                if (err != ERR_OK) {
                    if (buf != NULL) netbuf_delete(buf);
                    break;
                }
                if (buf != NULL) {
                    ok = consume_command_netbuf(client, buf, command_line,
                                                &command_len,
                                                sizeof(command_line),
                                                &authenticated);
                    netbuf_delete(buf);
                }
            }

            taskENTER_CRITICAL(); ++s_stats.disconnect_count; taskEXIT_CRITICAL();
            close_client(&client);
            GW_LOGI("UPLINK", "client disconnected");
        }

        if (listener != NULL) {
            (void)netconn_delete(listener);
            listener = NULL;
        }
        stats_set_listening(false);
        (void)xEventGroupClearBits(g_system_events, EVT_UPLINK_SERVER_READY);
    }
}

void gw_uplink_init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_history, 0, sizeof(s_history));
    s_history_head = 0U;
    s_history_count = 0U;
    s_event_sequence = 0U;
    s_frame_q = xQueueCreateStatic(UPLINK_FRAME_QUEUE_LEN,
                                   sizeof(gw_uplink_frame_t),
                                   s_frame_q_storage, &s_frame_q_cb);
    configASSERT(s_frame_q != NULL);
    s_alert_q=xQueueCreateStatic(UPLINK_ALERT_QUEUE_LEN,sizeof(gw_uplink_alert_t),s_alert_q_storage,&s_alert_q_cb);
    configASSERT(s_alert_q!=NULL);
}

void gw_uplink_task_create(void)
{
    TaskHandle_t handle = xTaskCreateStatic(uplink_task, "uplink",
                                            UPLINK_TASK_STACK_WORDS, NULL,
                                            UPLINK_TASK_PRIORITY,
                                            s_stack, &s_task_cb);
    configASSERT(handle != NULL);
}

void gw_uplink_get_stats(gw_uplink_stats_t *out)
{
    if (out == NULL) return;
    taskENTER_CRITICAL();
    *out = s_stats;
    taskEXIT_CRITICAL();
}

uint32_t gw_uplink_history_snapshot(gw_uplink_event_t *out, uint32_t max_count)
{
    if ((out == NULL) || (max_count == 0U)) return 0U;
    taskENTER_CRITICAL();
    uint32_t count = s_history_count;
    if (count > max_count) count = max_count;
    uint32_t start = (s_history_head + GW_UPLINK_HISTORY_DEPTH - count) %
                     GW_UPLINK_HISTORY_DEPTH;
    for (uint32_t i = 0U; i < count; ++i) {
        out[i] = s_history[(start + i) % GW_UPLINK_HISTORY_DEPTH];
    }
    taskEXIT_CRITICAL();
    return count;
}

#else

void gw_uplink_init(void) {}
void gw_uplink_task_create(void) {}
void gw_uplink_get_stats(gw_uplink_stats_t *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
}
void gw_uplink_publish_event(const gw_uplink_event_t *event) { (void)event; }
void gw_uplink_publish_can(const canfd_frame_t *frame, bool tx_direction)
{
    (void)frame; (void)tx_direction;
}
void gw_uplink_publish_modbus(const uint8_t *frame, uint16_t length,
                              uint32_t device_id, uint8_t slave,
                              bool tx_direction, gw_err_t result)
{
    (void)frame; (void)length; (void)device_id; (void)slave;
    (void)tx_direction; (void)result;
}
uint32_t gw_uplink_history_snapshot(gw_uplink_event_t *out, uint32_t max_count)
{
    (void)out; (void)max_count; return 0U;
}

#endif
