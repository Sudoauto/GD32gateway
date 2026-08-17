#include "gw_security.h"

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "gd32h7xx.h"
#include "gd32h7xx_fmc.h"
#include "gateway_build_config.h"
#include "gw_crypto.h"
#include "gw_time.h"
#include "rtos_objects.h"

static gw_security_credential_t s_credential;
static gw_security_stats_t s_stats;
static uint32_t s_consecutive_fail;
static uint32_t s_credential_revision;
static uint64_t s_hmi_last_activity;

static void hash_password(const uint8_t salt[GW_SECURITY_SALT_LEN], const char *password,
                          uint8_t out[GW_SECURITY_HASH_LEN])
{
    gw_sha256_two(salt,GW_SECURITY_SALT_LEN,password,strlen(password),out);
}

static void set_default_credential(void)
{
    memset(&s_credential,0,sizeof(s_credential));
    strncpy(s_credential.username,GW_AUTH_DEFAULT_USER,sizeof(s_credential.username)-1U);
    /* A salt need not be secret. Keep the default deterministic so factory
     * reset is reproducible; persisted credentials may replace it later. */
    static const uint8_t salt[GW_SECURITY_SALT_LEN]={0x47,0x44,0x33,0x32,0x48,0x37,0x35,0x39,0x02,0x47,0x44,0x32,0x48,0x01,0x09,0x00};
    memcpy(s_credential.salt,salt,sizeof(salt));
    hash_password(s_credential.salt,GW_AUTH_DEFAULT_PASSWORD,s_credential.password_hash);
    s_stats.default_password_active=true;
}

void gw_security_init(void)
{
    taskENTER_CRITICAL();
    memset(&s_stats,0,sizeof(s_stats));s_consecutive_fail=0U;s_hmi_last_activity=0U;set_default_credential();s_credential_revision=1U;
    taskEXIT_CRITICAL();
}

bool gw_security_authenticate(const char *username,const char *password)
{
#if (GW_AUTH_ENABLE == 0U)
    (void)username;(void)password;return true;
#else
    if((username==NULL)||(password==NULL))return false;
    gw_security_credential_t credential;uint32_t revision;uint64_t lockout_until;uint64_t now=gw_time_ms();
    taskENTER_CRITICAL();credential=s_credential;revision=s_credential_revision;lockout_until=s_stats.lockout_until_ms;taskEXIT_CRITICAL();
    if(now<lockout_until){taskENTER_CRITICAL();++s_stats.auth_fail;taskEXIT_CRITICAL();return false;}
    uint8_t digest[GW_SECURITY_HASH_LEN];hash_password(credential.salt,password,digest);
    bool match=(strncmp(username,credential.username,sizeof(credential.username))==0)&&gw_crypto_equal(digest,credential.password_hash,sizeof(digest));
    memset(digest,0,sizeof(digest));
    taskENTER_CRITICAL();
    /* A concurrent password/config update invalidates this authentication
     * attempt instead of allowing one final login against stale credentials. */
    if(revision!=s_credential_revision)match=false;
    if(match){++s_stats.auth_ok;s_consecutive_fail=0U;taskEXIT_CRITICAL();return true;}
    ++s_stats.auth_fail;++s_consecutive_fail;
    if(s_consecutive_fail>=GW_AUTH_MAX_FAILURES){s_stats.lockout_until_ms=now+GW_AUTH_LOCKOUT_MS;s_consecutive_fail=0U;++s_stats.lockout_count;}
    taskEXIT_CRITICAL();
    return false;
#endif
}

bool gw_security_hmi_authenticate(const char *username,const char *password)
{
    if(!gw_security_authenticate(username,password)) return false;
    taskENTER_CRITICAL();s_stats.hmi_unlocked=true;s_hmi_last_activity=gw_time_ms();taskEXIT_CRITICAL();
    if(g_system_events!=NULL)(void)xEventGroupSetBits(g_system_events,EVT_AUTH_UNLOCKED);
    return true;
}

gw_err_t gw_security_set_password(const char *username,const char *new_password)
{
    if((username==NULL)||(new_password==NULL)||(username[0]=='\0')||(strlen(username)>=GW_SECURITY_USER_MAX)||(strlen(new_password)<8U)||(strlen(new_password)>63U))return GW_ERR_PARAM;
    gw_security_credential_t next;memset(&next,0,sizeof(next));strncpy(next.username,username,sizeof(next.username)-1U);
    uint64_t mix=gw_time_ms() ^ ((uint64_t)SCB->CPUID<<32U);
    for(uint32_t i=0U;i<GW_SECURITY_SALT_LEN;++i){mix^=mix<<13;mix^=mix>>7;mix^=mix<<17;next.salt[i]=(uint8_t)(mix>>(i&7U));}
    hash_password(next.salt,new_password,next.password_hash);
    taskENTER_CRITICAL();s_credential=next;++s_credential_revision;if(s_credential_revision==0U)s_credential_revision=1U;s_stats.default_password_active=false;taskEXIT_CRITICAL();
    return GW_OK;
}

void gw_security_get_credential(gw_security_credential_t *out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_credential;taskEXIT_CRITICAL();}
gw_err_t gw_security_set_credential(const gw_security_credential_t *c){if((c==NULL)||(c->username[0]=='\0'))return GW_ERR_PARAM;taskENTER_CRITICAL();s_credential=*c;s_credential.username[GW_SECURITY_USER_MAX-1U]='\0';++s_credential_revision;if(s_credential_revision==0U)s_credential_revision=1U;s_stats.default_password_active=false;taskEXIT_CRITICAL();return GW_OK;}
void gw_security_hmi_lock(void){taskENTER_CRITICAL();s_stats.hmi_unlocked=false;s_hmi_last_activity=0U;taskEXIT_CRITICAL();if(g_system_events!=NULL)(void)xEventGroupClearBits(g_system_events,EVT_AUTH_UNLOCKED);}
bool gw_security_hmi_unlocked(void){if(GW_AUTH_ENABLE==0U)return true;bool unlocked;taskENTER_CRITICAL();unlocked=s_stats.hmi_unlocked;taskEXIT_CRITICAL();return unlocked;}
void gw_security_touch_session(void){taskENTER_CRITICAL();if(s_stats.hmi_unlocked)s_hmi_last_activity=gw_time_ms();taskEXIT_CRITICAL();}
void gw_security_periodic(void){bool lock=false;uint64_t now=gw_time_ms();taskENTER_CRITICAL();if(s_stats.hmi_unlocked&&(s_hmi_last_activity!=0U)&&(now-s_hmi_last_activity>GW_AUTH_SESSION_IDLE_MS))lock=true;taskEXIT_CRITICAL();if(lock)gw_security_hmi_lock();}
void gw_security_get_stats(gw_security_stats_t *out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_stats;taskEXIT_CRITICAL();}

gw_err_t gw_security_apply_production_lock(void)
{
#if (GW_PRODUCTION_LOCK_ENABLE == 0U)
    return GW_ERR_NOT_SUPPORTED;
#else
    if(GW_PRODUCTION_LOCK_CONFIRM!=0x4C315250UL) return GW_ERR_STATE; /* 'L1RP' */
    ob_unlock();fmc_state_enum e=ob_security_protection_config(FMC_LSPC);if(e==FMC_READY)e=ob_start();ob_lock();
    return (e==FMC_READY)?GW_OK:GW_ERR_IO;
#endif
}
