#ifndef GW_LWIP_ARCH_CC_H
#define GW_LWIP_ARCH_CC_H

#include <stdint.h>
#include "arch/cpu.h"

#define U16_F "hu"
#define S16_F "d"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"

#if defined(__ICCARM__)
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_USE_INCLUDES
#elif defined(__CC_ARM)
#define PACK_STRUCT_BEGIN __packed
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#elif defined(__GNUC__) || defined(__clang__)
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#else
#error "Unsupported compiler for lwIP packing"
#endif

uint32_t gw_lwip_rand(void);
void gw_lwip_platform_assert(const char *message, const char *file, int line);

/* Randomness here is for lwIP transaction IDs / ephemeral ports, not TLS keys.
 * A future TLS layer must use the GD32 TRNG through its own cryptographic RNG. */
#define LWIP_RAND() ((uint32_t)gw_lwip_rand())
#define LWIP_PLATFORM_ASSERT(message) \
    gw_lwip_platform_assert((message), __FILE__, __LINE__)

#endif /* GW_LWIP_ARCH_CC_H */
