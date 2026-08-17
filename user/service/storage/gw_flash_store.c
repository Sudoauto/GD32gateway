#include "gw_flash_store.h"

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "gd32h7xx.h"
#include "gd32h7xx_fmc.h"
#include "gw_crypto.h"

#define CFG_MAGIC       0x47434647UL /* GCFG */
#define CFG_VERSION     0x00090000UL
#define SPOOL_MAGIC     0x4753504CUL /* GSPL */
#define SPOOL_VERSION   1U
#define SPOOL_SLOT_SIZE 1024U
#define SPOOL_SLOTS     (GW_NVM_SPOOL_SIZE / SPOOL_SLOT_SIZE)
#define SPOOL_SLOTS_PER_ERASE (GW_NVM_ERASE_GRANULE / SPOOL_SLOT_SIZE)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint32_t length;
    uint32_t crc32;
    uint32_t header_crc32;
    uint32_t reserved[2];
} config_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t sequence;
    uint64_t timestamp_ms;
    uint16_t length;
    uint16_t reserved0;
    uint32_t payload_crc32;
    uint32_t header_crc32;
    uint8_t payload[GW_SPOOL_PAYLOAD_MAX];
    uint8_t pad[SPOOL_SLOT_SIZE - 32U - GW_SPOOL_PAYLOAD_MAX];
} spool_slot_t;

typedef char spool_slot_must_be_1024[(sizeof(spool_slot_t) == SPOOL_SLOT_SIZE) ? 1 : -1];

static StaticSemaphore_t s_mutex_cb;
static SemaphoreHandle_t s_mutex;
static gw_flash_store_stats_t s_stats;
static uint32_t s_spool_next_sequence;
static uint32_t s_spool_oldest_sequence;
static uint32_t s_spool_count;

static bool address_ok(uint32_t address, size_t length)
{
    uint64_t end=(uint64_t)address+(uint64_t)length;
    return (address>=GW_NVM_BASE)&&(end<=GW_NVM_END)&&(end>=(uint64_t)address);
}

static bool erased_region(uint32_t address, size_t length)
{
    const uint32_t *p=(const uint32_t *)(uintptr_t)address;
    size_t words=(length+3U)/4U;
    for(size_t i=0U;i<words;++i){if(p[i]!=0xFFFFFFFFU)return false;}
    return true;
}

static gw_err_t flash_erase_range(uint32_t address, size_t length)
{
    if (!address_ok(address,length) || ((address % GW_NVM_ERASE_GRANULE)!=0U)) return GW_ERR_PARAM;
    fmc_unlock();
    for(size_t off=0U;off<length;off+=GW_NVM_ERASE_GRANULE){
        if(fmc_sector_erase(address+(uint32_t)off)!=FMC_READY){fmc_lock();return GW_ERR_IO;}
    }
    fmc_lock(); __DSB(); __ISB(); return GW_OK;
}

static gw_err_t flash_program(uint32_t address,const void *data,size_t length)
{
    size_t programmed=(length+7U)&~((size_t)7U);
    if((data==NULL)||(length==0U)||!address_ok(address,programmed))return GW_ERR_PARAM;
    const uint8_t *src=(const uint8_t*)data;
    fmc_unlock();
    size_t off=0U;
    while(off<length){
        uint64_t word=UINT64_MAX;
        size_t n=length-off; if(n>8U)n=8U;
        memcpy(&word,&src[off],n);
        if(fmc_doubleword_program(address+(uint32_t)off,word)!=FMC_READY){fmc_lock();return GW_ERR_IO;}
        off+=8U;
    }
    fmc_lock(); __DSB(); __ISB(); return GW_OK;
}

static uint32_t config_header_crc(config_header_t h)
{
    h.header_crc32=0U; return gw_crc32(&h,sizeof(h));
}

static bool config_slot_valid(uint32_t base,config_header_t *out)
{
    const config_header_t *h=(const config_header_t *)(uintptr_t)base;
    if((h->magic!=CFG_MAGIC)||(h->version!=CFG_VERSION)||(h->length==0U)||(h->length>GW_FLASH_CONFIG_MAX))return false;
    config_header_t copy=*h;
    if(config_header_crc(copy)!=h->header_crc32)return false;
    const uint8_t *payload=(const uint8_t *)(uintptr_t)(base+sizeof(config_header_t));
    if(gw_crc32(payload,h->length)!=h->crc32)return false;
    if(out!=NULL)*out=*h; return true;
}

static uint32_t spool_header_crc(spool_slot_t s)
{
    s.header_crc32=0U; s.reserved0=0U; memset(s.payload,0,sizeof(s.payload));
    return gw_crc32(&s,32U);
}
static bool spool_slot_valid(uint32_t idx,spool_slot_t *out)
{
    if(idx>=SPOOL_SLOTS)return false;
    const spool_slot_t *s=(const spool_slot_t *)(uintptr_t)(GW_NVM_SPOOL_BASE+idx*SPOOL_SLOT_SIZE);
    if((s->magic!=SPOOL_MAGIC)||(s->version!=SPOOL_VERSION)||(s->kind!=GW_SPOOL_KIND_JSON)||
       (s->sequence==0U)||(s->reserved0!=0xFFFFU)||(s->length==0U)||(s->length>GW_SPOOL_PAYLOAD_MAX))return false;
    spool_slot_t c=*s;
    if(spool_header_crc(c)!=s->header_crc32)return false;
    if(gw_crc32(s->payload,s->length)!=s->payload_crc32)return false;
    if(out!=NULL)*out=*s; return true;
}

static void spool_scan(void)
{
    uint32_t min_seq=UINT32_MAX,max_seq=0U,count=0U;
    for(uint32_t i=0U;i<SPOOL_SLOTS;++i){spool_slot_t s;if(spool_slot_valid(i,&s)){++count;if(s.sequence<min_seq)min_seq=s.sequence;if(s.sequence>max_seq)max_seq=s.sequence;}}
    s_spool_count=count;
    s_spool_oldest_sequence=(count==0U)?0U:min_seq;
    s_spool_next_sequence=(max_seq==UINT32_MAX)?1U:(max_seq+1U);
    if(s_spool_next_sequence==0U)s_spool_next_sequence=1U;
    s_stats.spool_valid_count=count;
}

void gw_flash_store_init(void)
{
    memset(&s_stats,0,sizeof(s_stats));
    s_mutex=xSemaphoreCreateMutexStatic(&s_mutex_cb); configASSERT(s_mutex!=NULL);
    config_header_t a,b; bool va=config_slot_valid(GW_NVM_CONFIG_A,&a); bool vb=config_slot_valid(GW_NVM_CONFIG_B,&b);
    s_stats.config_valid=va||vb; s_stats.config_generation=(va&&vb)?((a.generation>b.generation)?a.generation:b.generation):(va?a.generation:(vb?b.generation:0U));
    spool_scan();
}

gw_err_t gw_flash_config_load(void *out,size_t capacity,size_t *length,uint32_t *generation)
{
    if((out==NULL)||(length==NULL)||(capacity==0U)||(s_mutex==NULL))return GW_ERR_PARAM;
    if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(50U))!=pdTRUE)return GW_ERR_BUSY;
    config_header_t a,b; bool va=config_slot_valid(GW_NVM_CONFIG_A,&a); bool vb=config_slot_valid(GW_NVM_CONFIG_B,&b);
    uint32_t base=0U; config_header_t h;
    if(va&&vb){if(a.generation>=b.generation){base=GW_NVM_CONFIG_A;h=a;}else{base=GW_NVM_CONFIG_B;h=b;}}
    else if(va){base=GW_NVM_CONFIG_A;h=a;} else if(vb){base=GW_NVM_CONFIG_B;h=b;} else {(void)xSemaphoreGive(s_mutex);return GW_ERR_NOT_FOUND;}
    if(h.length>capacity){(void)xSemaphoreGive(s_mutex);return GW_ERR_FULL;}
    memcpy(out,(const void *)(uintptr_t)(base+sizeof(config_header_t)),h.length); *length=h.length; if(generation!=NULL)*generation=h.generation;
    (void)xSemaphoreGive(s_mutex); return GW_OK;
}

gw_err_t gw_flash_config_save(const void *data,size_t length,uint32_t generation)
{
    if((data==NULL)||(length==0U)||(length>GW_FLASH_CONFIG_MAX)||(s_mutex==NULL))return GW_ERR_PARAM;
    if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(100U))!=pdTRUE)return GW_ERR_BUSY;
    config_header_t a,b; bool va=config_slot_valid(GW_NVM_CONFIG_A,&a); bool vb=config_slot_valid(GW_NVM_CONFIG_B,&b);
    uint32_t target;
    if(!va)target=GW_NVM_CONFIG_A; else if(!vb)target=GW_NVM_CONFIG_B; else target=(a.generation<=b.generation)?GW_NVM_CONFIG_A:GW_NVM_CONFIG_B;
    config_header_t h; memset(&h,0,sizeof(h)); h.magic=CFG_MAGIC;h.version=CFG_VERSION;h.generation=generation;h.length=(uint32_t)length;h.crc32=gw_crc32(data,length);h.header_crc32=config_header_crc(h);
    gw_err_t err=flash_erase_range(target,GW_NVM_CONFIG_SLOT_SIZE);
    if(err==GW_OK)err=flash_program(target+sizeof(h),data,length);
    if(err==GW_OK)err=flash_program(target,&h,sizeof(h)); /* header last = atomic commit */
    if(err==GW_OK){++s_stats.config_save_count;s_stats.config_valid=true;s_stats.config_generation=generation;}else{++s_stats.config_error_count;}
    (void)xSemaphoreGive(s_mutex);return err;
}

gw_err_t gw_flash_config_factory_reset(void)
{
    if(s_mutex==NULL)return GW_ERR_STATE;if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(100U))!=pdTRUE)return GW_ERR_BUSY;
    gw_err_t a=flash_erase_range(GW_NVM_CONFIG_A,GW_NVM_CONFIG_SLOT_SIZE);gw_err_t b=flash_erase_range(GW_NVM_CONFIG_B,GW_NVM_CONFIG_SLOT_SIZE);
    s_stats.config_valid=false;s_stats.config_generation=0U;(void)xSemaphoreGive(s_mutex);return (a==GW_OK&&b==GW_OK)?GW_OK:GW_ERR_IO;
}

gw_err_t gw_flash_spool_append(const void *data,uint16_t length,uint64_t timestamp_ms,uint32_t *sequence_out)
{
    if((data==NULL)||(length==0U)||(length>GW_SPOOL_PAYLOAD_MAX)||(s_mutex==NULL))return GW_ERR_PARAM;
    if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(50U))!=pdTRUE)return GW_ERR_BUSY;
    uint32_t seq=s_spool_next_sequence; uint32_t idx=(seq-1U)%SPOOL_SLOTS; uint32_t addr=GW_NVM_SPOOL_BASE+idx*SPOOL_SLOT_SIZE;
    bool erased_block=false;
    if((idx%SPOOL_SLOTS_PER_ERASE)==0U){
        if(flash_erase_range(addr,GW_NVM_ERASE_GRANULE)!=GW_OK){++s_stats.spool_drop_count;(void)xSemaphoreGive(s_mutex);return GW_ERR_IO;}
        erased_block=true;
    } else if(!erased_region(addr,SPOOL_SLOT_SIZE)){
        /* Power-loss or a non-aligned old image: erase the whole local block. */
        uint32_t block=addr-(addr%GW_NVM_ERASE_GRANULE);
        if(flash_erase_range(block,GW_NVM_ERASE_GRANULE)!=GW_OK){++s_stats.spool_drop_count;(void)xSemaphoreGive(s_mutex);return GW_ERR_IO;}
        erased_block=true;
    }
    if(erased_block){
        /* One hardware erase block contains four logical records. On ring wrap
         * these are the oldest records. Re-scan after erase so count/oldest are
         * exact instead of pretending only one slot was replaced. */
        spool_scan();
        s_spool_next_sequence=seq;
    }
    spool_slot_t s; memset(&s,0xFF,sizeof(s)); s.magic=SPOOL_MAGIC;s.version=SPOOL_VERSION;s.kind=GW_SPOOL_KIND_JSON;s.sequence=seq;s.timestamp_ms=timestamp_ms;s.length=length;s.reserved0=0xFFFFU;memcpy(s.payload,data,length);s.payload_crc32=gw_crc32(data,length);s.header_crc32=spool_header_crc(s);
    gw_err_t err=flash_program(addr,&s,sizeof(s));
    if(err==GW_OK){s_spool_next_sequence=seq+1U;if(s_spool_next_sequence==0U)s_spool_next_sequence=1U;if(s_spool_count<SPOOL_SLOTS)++s_spool_count;if(s_spool_oldest_sequence==0U)s_spool_oldest_sequence=seq;++s_stats.spool_append_count;s_stats.spool_valid_count=s_spool_count;if(sequence_out!=NULL)*sequence_out=seq;}else{++s_stats.spool_drop_count;}
    (void)xSemaphoreGive(s_mutex);return err;
}

gw_err_t gw_flash_spool_peek(gw_spool_record_t *out)
{
    if((out==NULL)||(s_mutex==NULL))return GW_ERR_PARAM;if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(20U))!=pdTRUE)return GW_ERR_BUSY;
    if(s_spool_count==0U){(void)xSemaphoreGive(s_mutex);return GW_ERR_NOT_FOUND;}
    uint32_t best_seq=UINT32_MAX;spool_slot_t best;bool found=false;
    for(uint32_t i=0U;i<SPOOL_SLOTS;++i){spool_slot_t s;if(spool_slot_valid(i,&s)&&s.sequence<best_seq){best_seq=s.sequence;best=s;found=true;}}
    if(!found){s_spool_count=0U;s_stats.spool_valid_count=0U;(void)xSemaphoreGive(s_mutex);return GW_ERR_NOT_FOUND;}
    memset(out,0,sizeof(*out));out->sequence=best.sequence;out->timestamp_ms=best.timestamp_ms;out->kind=(gw_spool_kind_t)best.kind;out->length=best.length;memcpy(out->payload,best.payload,best.length);
    (void)xSemaphoreGive(s_mutex);return GW_OK;
}

gw_err_t gw_flash_spool_pop(uint32_t expected_sequence)
{
    if((expected_sequence==0U)||(s_mutex==NULL))return GW_ERR_PARAM;if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(20U))!=pdTRUE)return GW_ERR_BUSY;
    bool found=false;for(uint32_t i=0U;i<SPOOL_SLOTS;++i){spool_slot_t s;if(spool_slot_valid(i,&s)&&s.sequence==expected_sequence){found=true;break;}}
    if(!found){(void)xSemaphoreGive(s_mutex);return GW_ERR_NOT_FOUND;}
    uint32_t slot_idx=(expected_sequence-1U)%SPOOL_SLOTS;
    uint32_t addr=GW_NVM_SPOOL_BASE+slot_idx*SPOOL_SLOT_SIZE+16U;
    uint64_t marker_word=*(const uint64_t *)(uintptr_t)addr;
    marker_word &= ~((uint64_t)0xFFFFU << 48U); /* reserved0 at bytes 22..23 */
    gw_err_t mark_err=flash_program(addr,&marker_word,sizeof(marker_word));
    if(mark_err==GW_OK){s_spool_oldest_sequence=expected_sequence+1U;if(s_spool_count>0U)--s_spool_count;++s_stats.spool_replay_count;s_stats.spool_valid_count=s_spool_count;}
    (void)xSemaphoreGive(s_mutex);return mark_err;
}

gw_err_t gw_flash_spool_clear(void)
{
    if(s_mutex==NULL)return GW_ERR_STATE;if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(100U))!=pdTRUE)return GW_ERR_BUSY;gw_err_t e=flash_erase_range(GW_NVM_SPOOL_BASE,GW_NVM_SPOOL_SIZE);if(e==GW_OK){s_spool_count=0U;s_spool_oldest_sequence=0U;s_spool_next_sequence=1U;s_stats.spool_valid_count=0U;}(void)xSemaphoreGive(s_mutex);return e;
}
uint32_t gw_flash_spool_count(void){return s_spool_count;}
void gw_flash_store_get_stats(gw_flash_store_stats_t *out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_stats;taskEXIT_CRITICAL();}
