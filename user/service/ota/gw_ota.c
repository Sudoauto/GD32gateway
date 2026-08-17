#include "gw_ota.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "gateway_build_config.h"
#include "gw_crypto.h"

static gw_ota_manifest_t s_manifest;static gw_ota_status_t s_status;
void gw_ota_init(void){memset(&s_manifest,0,sizeof(s_manifest));memset(&s_status,0,sizeof(s_status));}
gw_err_t gw_ota_begin(const gw_ota_manifest_t *m){
#if (GW_OTA_ENABLE==0U)
(void)m;return GW_ERR_NOT_SUPPORTED;
#else
if((m==NULL)||(m->image_size==0U)||(m->signature_length==0U)||(m->signature_length>GW_OTA_SIGNATURE_MAX))return GW_ERR_PARAM;
gw_err_t e=gw_ota_storage_begin(m->image_size,m->encrypted);if(e!=GW_OK){s_status.state=GW_OTA_ERROR;s_status.last_error=e;return e;}s_manifest=*m;s_status.state=GW_OTA_RECEIVING;s_status.expected_size=m->image_size;s_status.received=0U;s_status.last_error=GW_OK;return GW_OK;
#endif
}
gw_err_t gw_ota_write(uint32_t off,const uint8_t *data,uint32_t len){if((s_status.state!=GW_OTA_RECEIVING)||(data==NULL)||(len==0U)||(off!=s_status.received)||(off+len>s_status.expected_size))return GW_ERR_STATE;gw_err_t e=gw_ota_storage_write(off,data,len);if(e==GW_OK)s_status.received+=len;else{s_status.state=GW_OTA_ERROR;s_status.last_error=e;}return e;}
gw_err_t gw_ota_finalize(void){if((s_status.state!=GW_OTA_RECEIVING)||(s_status.received!=s_status.expected_size))return GW_ERR_STATE;s_status.state=GW_OTA_VERIFYING;gw_sha256_ctx_t ctx;gw_sha256_init(&ctx);uint8_t buf[256];for(uint32_t off=0U;off<s_status.expected_size;){uint32_t n=s_status.expected_size-off;if(n>sizeof(buf))n=sizeof(buf);gw_err_t e=gw_ota_storage_read(off,buf,n);if(e!=GW_OK){s_status.state=GW_OTA_ERROR;s_status.last_error=e;return e;}gw_sha256_update(&ctx,buf,n);off+=n;}uint8_t digest[32];gw_sha256_final(&ctx,digest);if(!gw_crypto_equal(digest,s_manifest.image_sha256,32U)||!gw_secure_verify_signature(digest,s_manifest.signature,s_manifest.signature_length)){s_status.state=GW_OTA_ERROR;s_status.last_error=GW_ERR_PROTOCOL;return GW_ERR_PROTOCOL;}gw_err_t e=gw_secure_mark_image_pending(s_manifest.version,s_manifest.image_size,digest);if(e==GW_OK){s_status.state=GW_OTA_READY;s_status.last_error=GW_OK;}else{s_status.state=GW_OTA_ERROR;s_status.last_error=e;}return e;}
void gw_ota_abort(void){gw_ota_storage_abort();memset(&s_manifest,0,sizeof(s_manifest));memset(&s_status,0,sizeof(s_status));}
void gw_ota_get_status(gw_ota_status_t *out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_status;taskEXIT_CRITICAL();}

__attribute__((weak)) gw_err_t gw_ota_storage_begin(uint32_t s,bool e){(void)s;(void)e;return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) gw_err_t gw_ota_storage_write(uint32_t o,const uint8_t*d,uint32_t l){(void)o;(void)d;(void)l;return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) gw_err_t gw_ota_storage_read(uint32_t o,uint8_t*d,uint32_t l){(void)o;(void)d;(void)l;return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) void gw_ota_storage_abort(void){}
__attribute__((weak)) bool gw_secure_verify_signature(const uint8_t d[32],const uint8_t*s,uint16_t l){(void)d;(void)s;(void)l;return false;}
__attribute__((weak)) gw_err_t gw_secure_mark_image_pending(uint32_t v,uint32_t s,const uint8_t d[32]){(void)v;(void)s;(void)d;return GW_ERR_NOT_SUPPORTED;}
