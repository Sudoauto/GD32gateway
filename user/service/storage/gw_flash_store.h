#ifndef GW_FLASH_STORE_H
#define GW_FLASH_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gw_error.h"

/* Keil IROM is intentionally capped at 0x0837FFFF. The last 256 KiB of the
 * 3840 KiB device flash (0x08380000..0x083BFFFF)
 * is intentionally outside the application image and reserved for gateway
 * persistent data. The backend can later be replaced by SD/OSPI without
 * changing the configuration/spool services. */
#define GW_NVM_BASE             0x08380000UL
#define GW_NVM_END              0x083C0000UL
#define GW_NVM_CONFIG_A         0x08380000UL
#define GW_NVM_CONFIG_B         0x08388000UL
#define GW_NVM_CONFIG_SLOT_SIZE 0x00008000UL
#define GW_NVM_SPOOL_BASE       0x08390000UL
#define GW_NVM_SPOOL_SIZE       0x00030000UL
#define GW_NVM_ERASE_GRANULE    0x00001000UL

#define GW_FLASH_CONFIG_MAX     28672U
#define GW_SPOOL_PAYLOAD_MAX    976U

typedef enum {
    GW_SPOOL_KIND_NONE = 0,
    GW_SPOOL_KIND_JSON = 1,
} gw_spool_kind_t;

typedef struct {
    uint32_t sequence;
    uint64_t timestamp_ms;
    gw_spool_kind_t kind;
    uint16_t length;
    uint8_t payload[GW_SPOOL_PAYLOAD_MAX];
} gw_spool_record_t;

typedef struct {
    bool config_valid;
    uint32_t config_generation;
    uint32_t config_save_count;
    uint32_t config_error_count;
    uint32_t spool_valid_count;
    uint32_t spool_append_count;
    uint32_t spool_drop_count;
    uint32_t spool_replay_count;
} gw_flash_store_stats_t;

void gw_flash_store_init(void);
gw_err_t gw_flash_config_load(void *out, size_t capacity, size_t *length,
                              uint32_t *generation);
gw_err_t gw_flash_config_save(const void *data, size_t length,
                              uint32_t generation);
gw_err_t gw_flash_config_factory_reset(void);

gw_err_t gw_flash_spool_append(const void *data, uint16_t length,
                               uint64_t timestamp_ms, uint32_t *sequence_out);
gw_err_t gw_flash_spool_peek(gw_spool_record_t *out);
gw_err_t gw_flash_spool_pop(uint32_t expected_sequence);
gw_err_t gw_flash_spool_clear(void);
uint32_t gw_flash_spool_count(void);
void gw_flash_store_get_stats(gw_flash_store_stats_t *out);

#endif
