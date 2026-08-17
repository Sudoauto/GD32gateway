#ifndef GW_LWIP_PORT_H
#define GW_LWIP_PORT_H

#include <stdint.h>

uint32_t gw_lwip_rand(void);
void gw_lwip_platform_assert(const char *message, const char *file, int line);

#endif /* GW_LWIP_PORT_H */
