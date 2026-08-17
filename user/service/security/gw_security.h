#ifndef GW_SECURITY_H
#define GW_SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gw_error.h"

#define GW_SECURITY_USER_MAX 24U
#define GW_SECURITY_SALT_LEN 16U
#define GW_SECURITY_HASH_LEN 32U

typedef struct {
    char username[GW_SECURITY_USER_MAX];
    uint8_t salt[GW_SECURITY_SALT_LEN];
    uint8_t password_hash[GW_SECURITY_HASH_LEN];
} gw_security_credential_t;

typedef struct {
    uint32_t auth_ok;
    uint32_t auth_fail;
    uint32_t lockout_count;
    uint64_t lockout_until_ms;
    bool hmi_unlocked;
    bool default_password_active;
} gw_security_stats_t;

void gw_security_init(void);
bool gw_security_authenticate(const char *username, const char *password);
bool gw_security_hmi_authenticate(const char *username, const char *password);
gw_err_t gw_security_set_password(const char *username, const char *new_password);
void gw_security_get_credential(gw_security_credential_t *out);
gw_err_t gw_security_set_credential(const gw_security_credential_t *credential);
void gw_security_hmi_lock(void);
bool gw_security_hmi_unlocked(void);
void gw_security_touch_session(void);
void gw_security_periodic(void);
void gw_security_get_stats(gw_security_stats_t *out);

/* Product-release read protection. Default build is disabled. Only Level-1 is
 * automated here because Level-2/high protection is potentially irreversible
 * and belongs in a controlled manufacturing provisioning process. */
gw_err_t gw_security_apply_production_lock(void);

#endif
