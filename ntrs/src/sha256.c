#include "sha256.h"

#include <string.h>

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t Rotr32(uint32_t value, uint32_t shift) { return (value >> shift) | (value << (32u - shift)); }

static uint32_t Load32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void Store32be(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)((value >> 24) & 0xFFu);
    p[1] = (uint8_t)((value >> 16) & 0xFFu);
    p[2] = (uint8_t)((value >> 8) & 0xFFu);
    p[3] = (uint8_t)(value & 0xFFu);
}

static void Store64be(uint8_t *p, uint64_t value)
{
    int i;
    for (i = 7; i >= 0; --i) {
        p[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

static void Sha256Transform(SHA256_CTX *ctx, const uint8_t block[SHA256_BLOCK_LENGTH])
{
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t i;

    for (i = 0; i < 16; ++i) {
        w[i] = Load32be(block + i * 4u);
    }
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        uint32_t s1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        uint32_t s0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void SHA256_Init(SHA256_CTX *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bit_len = 0;
    ctx->buffer_len = 0;
}

void SHA256_Update(SHA256_CTX *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (len == 0) {
        return;
    }

    ctx->bit_len += (uint64_t)len * 8u;
    while (len > 0) {
        size_t copy_len = sizeof(ctx->buffer) - ctx->buffer_len;
        if (copy_len > len) {
            copy_len = len;
        }
        memcpy(ctx->buffer + ctx->buffer_len, p, copy_len);
        ctx->buffer_len += copy_len;
        p += copy_len;
        len -= copy_len;
        if (ctx->buffer_len == sizeof(ctx->buffer)) {
            Sha256Transform(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

void SHA256_Final(uint8_t *digest, SHA256_CTX *ctx)
{
    size_t i;

    ctx->buffer[ctx->buffer_len++] = 0x80u;
    if (ctx->buffer_len > 56u) {
        while (ctx->buffer_len < sizeof(ctx->buffer)) {
            ctx->buffer[ctx->buffer_len++] = 0;
        }
        Sha256Transform(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }
    while (ctx->buffer_len < 56u) {
        ctx->buffer[ctx->buffer_len++] = 0;
    }
    Store64be(ctx->buffer + 56, ctx->bit_len);
    Sha256Transform(ctx, ctx->buffer);
    for (i = 0; i < 8; ++i) {
        Store32be(digest + i * 4u, ctx->state[i]);
    }

    memset(ctx, 0, sizeof(*ctx));
}
