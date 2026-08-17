#ifndef GW_OTA_H
#define GW_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gw_error.h"

#define GW_OTA_SIGNATURE_MAX 72U

typedef struct {
    uint32_t version;
    uint32_t image_size;
    uint8_t image_sha256[32];
    uint16_t signature_length;
    uint8_t signature[GW_OTA_SIGNATURE_MAX];
    bool encrypted;
} gw_ota_manifest_t;

typedef enum { GW_OTA_IDLE=0,GW_OTA_RECEIVING,GW_OTA_VERIFYING,GW_OTA_READY,GW_OTA_ERROR } gw_ota_state_t;
typedef struct {gw_ota_state_t state;uint32_t expected_size;uint32_t received;gw_err_t last_error;} gw_ota_status_t;

void gw_ota_init(void);
gw_err_t gw_ota_begin(const gw_ota_manifest_t *manifest);
gw_err_t gw_ota_write(uint32_t offset,const uint8_t *data,uint32_t length);
gw_err_t gw_ota_finalize(void);
void gw_ota_abort(void);
void gw_ota_get_status(gw_ota_status_t *out);

/* Board/bootloader integration points. The default implementation is fail-
 * closed. A production board must provide external staging storage, signature
 * verification and a bootloader hand-off/rollback implementation. */
gw_err_t gw_ota_storage_begin(uint32_t image_size,bool encrypted);
gw_err_t gw_ota_storage_write(uint32_t offset,const uint8_t *data,uint32_t length);
gw_err_t gw_ota_storage_read(uint32_t offset,uint8_t *data,uint32_t length);
void gw_ota_storage_abort(void);
bool gw_secure_verify_signature(const uint8_t digest[32],const uint8_t *signature,uint16_t signature_length);
gw_err_t gw_secure_mark_image_pending(uint32_t version,uint32_t image_size,const uint8_t digest[32]);

#endif
