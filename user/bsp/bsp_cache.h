#ifndef BSP_CACHE_H
#define BSP_CACHE_H

#include <stddef.h>

void bsp_cache_enable(void);
void bsp_dcache_clean(const void *address, size_t length);
void bsp_dcache_invalidate(const void *address, size_t length);
void bsp_dcache_clean_invalidate(const void *address, size_t length);

#endif
