#ifndef GW_CONFIG_H
#define GW_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gw_error.h"
#include "gw_security.h"

typedef struct {
    bool dhcp;
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gateway[4];
    uint32_t rs485_baudrate;
    uint8_t rs485_data_bits;
    uint8_t rs485_stop_bits;
    uint8_t rs485_parity;
    uint32_t rs485_timeout_ms;
    uint8_t rs485_retry;
    char sntp_server[64];
    char syslog_server[40];
    uint16_t syslog_port;
    uint8_t syslog_min_level; /* 0=error, 1=warning */
    char snmp_community[24];
} gw_runtime_config_t;

typedef struct {
    bool loaded_from_persistent;
    bool loaded_from_external;
    bool dirty;
    uint32_t generation;
    uint32_t load_count;
    uint32_t save_count;
    uint32_t parse_error_count;
    uint32_t rejected_row_count;
    uint32_t factory_reset_count;
    uint32_t csv_length;
} gw_config_stats_t;

void gw_config_init(void);
void gw_config_task_create(void);
void gw_config_get_runtime(gw_runtime_config_t *out);
void gw_config_get_stats(gw_config_stats_t *out);

/* Replace the runtime model from a complete CSV document. All dynamic
 * devices/points/maps/polls/alarms/rules are rebuilt atomically from rows. */
gw_err_t gw_config_import_csv(const char *csv, size_t length, bool persist);
/* Apply one CSV row as an upsert. Useful for TCP/HTTP/MQTT remote changes. */
gw_err_t gw_config_apply_csv_line(const char *line, bool persist);
gw_err_t gw_config_delete(const char *kind, uint32_t id, bool persist);
size_t gw_config_export_csv(char *out, size_t capacity);
void gw_config_mark_dirty(void);
void gw_config_request_save(void);
void gw_config_request_factory_reset(bool clear_spool);

/* Optional SD/OSPI backend hooks. Returning GW_ERR_NOT_SUPPORTED selects the
 * internal dual-slot Flash store. A board port may override these weak symbols
 * to read/write /config.csv on removable or external nonvolatile storage. */
gw_err_t gw_config_external_load_csv(char *out,size_t capacity,size_t *length);
gw_err_t gw_config_external_save_csv(const char *data,size_t length);
/* Optional external backend cleanup. Factory reset calls this before reboot so
 * an SD/OSPI config file cannot silently restore the old configuration. */
gw_err_t gw_config_external_factory_reset(void);
/* Board override: return true while the physical factory-reset button is held. */
bool gw_factory_button_pressed(void);

#endif
