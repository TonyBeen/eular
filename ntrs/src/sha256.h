#ifndef __SHA256_H__
#define __SHA256_H__

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
    
#define SHA256_DIGEST_LENGTH    32
#define SHA256_BLOCK_LENGTH     64

typedef struct {
    uint32_t    state[8];
    uint64_t    bit_len;
    uint8_t     buffer[SHA256_BLOCK_LENGTH];
    size_t      buffer_len;
} SHA256_CTX;

void SHA256_Init(SHA256_CTX *ctx);
void SHA256_Update(SHA256_CTX *ctx, const void *data, size_t len);
void SHA256_Final(uint8_t *digest, SHA256_CTX *ctx);

#ifdef __cplusplus
};
#endif

#endif // __SHA256_H__
