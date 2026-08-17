#include "bsp_cache.h"
#include <stdint.h>
#include "gd32h7xx.h"
#include "gd32h7xx_misc.h"
#include "gw_lcd.h"

#define DCACHE_LINE_SIZE 32U

static void cache_bounds(const void *address, size_t length,
                         uintptr_t *start_out, int32_t *size_out)
{
    uintptr_t start = ((uintptr_t)address) & ~((uintptr_t)DCACHE_LINE_SIZE - 1U);
    uintptr_t end = ((uintptr_t)address + length + DCACHE_LINE_SIZE - 1U) &
                    ~((uintptr_t)DCACHE_LINE_SIZE - 1U);
    *start_out = start;
    *size_out = (int32_t)(end - start);
}

static void gateway_dma_mpu_config(void)
{
    /* GD32 ENET SPL places RX/TX descriptors and DMA buffers at
     * 0x30000000..0x30003FFF. Make this complete 16 KiB window non-cacheable
     * before D-cache is enabled. This follows the supplied H759/LAN8720A
     * FreeRTOS example and avoids descriptor/data coherency races. */
    mpu_region_init_struct cfg;
    ARM_MPU_Disable();
    mpu_region_struct_para_init(&cfg);
    cfg.region_base_address = 0x30000000UL;
    cfg.region_size = MPU_REGION_SIZE_16KB;
    cfg.access_permission = MPU_AP_FULL_ACCESS;
    cfg.access_bufferable = MPU_ACCESS_BUFFERABLE;
    cfg.access_cacheable = MPU_ACCESS_NON_CACHEABLE;
    cfg.access_shareable = MPU_ACCESS_NON_SHAREABLE;
    cfg.region_number = MPU_REGION_NUMBER0;
    cfg.subregion_disable = MPU_SUBREGION_ENABLE;
    cfg.instruction_exec = MPU_INSTRUCTION_EXEC_NOT_PERMIT;
    cfg.tex_type = MPU_TEX_TYPE0;
    mpu_region_config(&cfg);
    mpu_region_enable();

    /* Official 5-inch TLI/LVGL example uses SDRAM at 0xC0000000 for the
     * framebuffer.  Keep the first 4 MiB non-cacheable so TLI/IPA and the CPU
     * see the same RGB565 data without explicit cache maintenance. */
    mpu_region_struct_para_init(&cfg);
    cfg.region_base_address = 0xC0000000UL;
    cfg.region_size = MPU_REGION_SIZE_4MB;
    cfg.access_permission = MPU_AP_FULL_ACCESS;
    /* Cortex-M7 MPU encoding matters here. TEX=0,C=0,B=1 describes
     * Device memory, which is unsuitable for the LVGL TLSF heap because
     * ordinary allocator/object accesses may be unaligned. Use Normal,
     * non-cacheable memory instead: TEX=1,C=0,B=0. This remains coherent
     * for TLI/IPA while preserving normal RAM access semantics for LVGL. */
    cfg.access_bufferable = MPU_ACCESS_NON_BUFFERABLE;
    cfg.access_cacheable = MPU_ACCESS_NON_CACHEABLE;
    cfg.access_shareable = MPU_ACCESS_NON_SHAREABLE;
    cfg.region_number = MPU_REGION_NUMBER1;
    cfg.subregion_disable = MPU_SUBREGION_ENABLE;
    cfg.instruction_exec = MPU_INSTRUCTION_EXEC_NOT_PERMIT;
    cfg.tex_type = MPU_TEX_TYPE1;
    mpu_region_config(&cfg);
    mpu_region_enable();

    /* Overlay only the LVGL allocator pool as cacheable normal memory.
     * Framebuffers stay in the lower-priority non-cacheable SDRAM region so
     * TLI/IPA remain coherent without cache maintenance. LVGL object/style/
     * text allocations are CPU-only and benefit substantially from D-cache. */
    mpu_region_struct_para_init(&cfg);
    cfg.region_base_address = GW_LVGL_HEAP_ADDR;
    cfg.region_size = MPU_REGION_SIZE_512KB;
    cfg.access_permission = MPU_AP_FULL_ACCESS;
    cfg.access_bufferable = MPU_ACCESS_BUFFERABLE;
    cfg.access_cacheable = MPU_ACCESS_CACHEABLE;
    cfg.access_shareable = MPU_ACCESS_NON_SHAREABLE;
    cfg.region_number = MPU_REGION_NUMBER2;
    cfg.subregion_disable = MPU_SUBREGION_ENABLE;
    cfg.instruction_exec = MPU_INSTRUCTION_EXEC_NOT_PERMIT;
    /* TEX=0,C=1,B=1: normal write-back cacheable memory. Region 2 has higher
     * priority than the broad non-cacheable SDRAM region 1. */
    cfg.tex_type = MPU_TEX_TYPE0;
    mpu_region_config(&cfg);
    mpu_region_enable();

    ARM_MPU_Enable(MPU_MODE_PRIV_DEFAULT);
    __DSB();
    __ISB();
}

void bsp_cache_enable(void)
{
    gateway_dma_mpu_config();
    SCB_EnableICache();
    SCB_EnableDCache();
}

void bsp_dcache_clean(const void *address, size_t length)
{
    if ((address == NULL) || (length == 0U)) {
        return;
    }
    uintptr_t start;
    int32_t size;
    cache_bounds(address, length, &start, &size);
    SCB_CleanDCache_by_Addr((uint32_t *)start, size);
}

void bsp_dcache_invalidate(const void *address, size_t length)
{
    if ((address == NULL) || (length == 0U)) {
        return;
    }
    uintptr_t start;
    int32_t size;
    cache_bounds(address, length, &start, &size);
    SCB_InvalidateDCache_by_Addr((uint32_t *)start, size);
}

void bsp_dcache_clean_invalidate(const void *address, size_t length)
{
    if ((address == NULL) || (length == 0U)) {
        return;
    }
    uintptr_t start;
    int32_t size;
    cache_bounds(address, length, &start, &size);
    SCB_CleanDCache_by_Addr((uint32_t *)start, size);
    SCB_InvalidateDCache_by_Addr((uint32_t *)start, size);
}
