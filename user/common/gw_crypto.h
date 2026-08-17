#ifndef GW_CRYPTO_H
#define GW_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_SHA256_BYTES 32U

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t block[64];
    uint32_t used;
} gw_sha256_ctx_t;

void gw_sha256_init(gw_sha256_ctx_t *ctx);
void gw_sha256_update(gw_sha256_ctx_t *ctx, const void *data, size_t len);
void gw_sha256_final(gw_sha256_ctx_t *ctx, uint8_t out[GW_SHA256_BYTES]);

void gw_sha256(const void *data, size_t len, uint8_t out[GW_SHA256_BYTES]);
void gw_sha256_two(const void *a, size_t a_len, const void *b, size_t b_len,
                   uint8_t out[GW_SHA256_BYTES]);
bool gw_crypto_equal(const uint8_t *a, const uint8_t *b, size_t len);
uint32_t gw_crc32(const void *data, size_t len);

#endif
