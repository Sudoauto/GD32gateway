#include "gw_crypto.h"

#include <string.h>


static const uint32_t k256[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32U - n)); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) |
           ((uint32_t)p[2] << 8U) | (uint32_t)p[3];
}
static void store_be32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24U); p[1]=(uint8_t)(v>>16U); p[2]=(uint8_t)(v>>8U); p[3]=(uint8_t)v;
}

static void sha256_compress(gw_sha256_ctx_t *c, const uint8_t block[64])
{
    uint32_t w[64];
    for (uint32_t i=0U;i<16U;++i) w[i]=be32(&block[i*4U]);
    for (uint32_t i=16U;i<64U;++i) {
        uint32_t s0=rotr(w[i-15U],7U)^rotr(w[i-15U],18U)^(w[i-15U]>>3U);
        uint32_t s1=rotr(w[i-2U],17U)^rotr(w[i-2U],19U)^(w[i-2U]>>10U);
        w[i]=w[i-16U]+s0+w[i-7U]+s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (uint32_t i=0U;i<64U;++i) {
        uint32_t S1=rotr(e,6U)^rotr(e,11U)^rotr(e,25U);
        uint32_t ch=(e&f)^((~e)&g);
        uint32_t t1=h+S1+ch+k256[i]+w[i];
        uint32_t S0=rotr(a,2U)^rotr(a,13U)^rotr(a,22U);
        uint32_t maj=(a&b)^(a&cc)^(b&cc);
        uint32_t t2=S0+maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

void gw_sha256_init(gw_sha256_ctx_t *c)
{
    static const uint32_t init[8]={0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    memcpy(c->h,init,sizeof(init)); c->total=0U; c->used=0U;
}
void gw_sha256_update(gw_sha256_ctx_t *c,const void *data,size_t len)
{
    const uint8_t *p=(const uint8_t*)data;
    if ((p==NULL)&&(len!=0U)) return;
    c->total += (uint64_t)len;
    while (len!=0U) {
        size_t take=64U-c->used; if (take>len) take=len;
        memcpy(&c->block[c->used],p,take); c->used+=(uint32_t)take; p+=take; len-=take;
        if (c->used==64U) { sha256_compress(c,c->block); c->used=0U; }
    }
}
void gw_sha256_final(gw_sha256_ctx_t *c,uint8_t out[32])
{
    uint64_t bits=c->total*8ULL;
    c->block[c->used++]=0x80U;
    if (c->used>56U) { while(c->used<64U)c->block[c->used++]=0U; sha256_compress(c,c->block); c->used=0U; }
    while(c->used<56U)c->block[c->used++]=0U;
    for(uint32_t i=0U;i<8U;++i)c->block[63U-i]=(uint8_t)(bits>>(8U*i));
    sha256_compress(c,c->block);
    for(uint32_t i=0U;i<8U;++i)store_be32(&out[i*4U],c->h[i]);
    memset(c,0,sizeof(*c));
}

void gw_sha256(const void *data,size_t len,uint8_t out[GW_SHA256_BYTES])
{
    gw_sha256_ctx_t c; if(out==NULL)return; gw_sha256_init(&c); gw_sha256_update(&c,data,len); gw_sha256_final(&c,out);
}
void gw_sha256_two(const void *a,size_t a_len,const void *b,size_t b_len,uint8_t out[GW_SHA256_BYTES])
{
    gw_sha256_ctx_t c; if(out==NULL)return; gw_sha256_init(&c); gw_sha256_update(&c,a,a_len); gw_sha256_update(&c,b,b_len); gw_sha256_final(&c,out);
}
bool gw_crypto_equal(const uint8_t *a,const uint8_t *b,size_t len)
{
    if((a==NULL)||(b==NULL))return false; uint8_t diff=0U; for(size_t i=0U;i<len;++i)diff|=(uint8_t)(a[i]^b[i]); return diff==0U;
}
uint32_t gw_crc32(const void *data,size_t len)
{
    const uint8_t *p=(const uint8_t*)data; uint32_t crc=0xFFFFFFFFU;
    if((p==NULL)&&(len!=0U))return 0U;
    for(size_t i=0U;i<len;++i){crc^=p[i];for(uint32_t b=0U;b<8U;++b)crc=(crc>>1U)^((0U-(crc&1U))&0xEDB88320U);} return ~crc;
}
