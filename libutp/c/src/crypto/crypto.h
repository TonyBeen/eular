#ifndef EULAR_UTP_INTERNAL_CRYPTO_H
#define EULAR_UTP_INTERNAL_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/allocator.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_CRYPTO_X25519_KEY_SIZE   32u
#define UTP_CRYPTO_AES_128_KEY_SIZE  16u
#define UTP_CRYPTO_AES_256_KEY_SIZE  32u
#define UTP_CRYPTO_MAX_KEY_SIZE      UTP_CRYPTO_AES_256_KEY_SIZE
#define UTP_CRYPTO_NONCE_PREFIX_SIZE 4u
#define UTP_CRYPTO_AEAD_NONCE_SIZE   12u
#define UTP_CRYPTO_AEAD_TAG_SIZE     16u

#define UTP_CRYPTO_TYPE_AES_GCM_128 0u
#define UTP_CRYPTO_TYPE_AES_GCM_256 1u

typedef struct utp_crypto_key_pair {
    uint8_t public_key[UTP_CRYPTO_X25519_KEY_SIZE];
    uint8_t private_key[UTP_CRYPTO_X25519_KEY_SIZE];
} utp_crypto_key_pair_t;

typedef struct utp_crypto_traffic_secret {
    uint8_t key[UTP_CRYPTO_MAX_KEY_SIZE];
    uint8_t nonce_prefix[UTP_CRYPTO_NONCE_PREFIX_SIZE];
} utp_crypto_traffic_secret_t;

typedef struct utp_crypto_traffic_material {
    utp_crypto_traffic_secret_t client_to_server;
    utp_crypto_traffic_secret_t server_to_client;
    size_t                      key_size;
} utp_crypto_traffic_material_t;

typedef struct utp_crypto_aead {
    void*                  provider_context;
    const utp_allocator_t* allocator;
    uint8_t                nonce_prefix[UTP_CRYPTO_NONCE_PREFIX_SIZE];
    size_t                 key_size;
} utp_crypto_aead_t;

void                 utp_crypto_key_pair_clear(utp_crypto_key_pair_t* key_pair);
utp_internal_error_t utp_crypto_key_pair_generate(utp_crypto_key_pair_t* key_pair);
utp_internal_error_t utp_crypto_key_pair_from_private(utp_crypto_key_pair_t* key_pair,
                                                      const uint8_t          private_key[UTP_CRYPTO_X25519_KEY_SIZE]);
utp_internal_error_t utp_crypto_x25519_derive(uint8_t       shared_secret[UTP_CRYPTO_X25519_KEY_SIZE],
                                              const uint8_t private_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                              const uint8_t peer_public_key[UTP_CRYPTO_X25519_KEY_SIZE]);
void                 utp_crypto_traffic_material_clear(utp_crypto_traffic_material_t* material);
utp_internal_error_t utp_crypto_derive_traffic_material(utp_crypto_traffic_material_t* material,
                                                        const uint8_t shared_secret[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        const uint8_t client_public_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        const uint8_t server_public_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        uint32_t client_cid, uint32_t server_cid, uint8_t crypto_type,
                                                        size_t key_size);
// 按 client/server 方向归一化转录，并创建本端独立的发送和接收 AEAD 上下文。
utp_internal_error_t utp_crypto_create_directional_aead(const utp_crypto_key_pair_t* local_key_pair,
                                                        const uint8_t peer_public_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        uint32_t client_cid, uint32_t server_cid, uint8_t crypto_type,
                                                        bool local_is_client, utp_crypto_aead_t* tx,
                                                        utp_crypto_aead_t* rx);
void                 utp_crypto_aead_cleanup(utp_crypto_aead_t* aead);
// 首次初始化前对象必须清零；再次初始化前必须已完成 cleanup。
utp_internal_error_t utp_crypto_aead_init(utp_crypto_aead_t* aead, const utp_allocator_t* allocator, const uint8_t* key,
                                          size_t key_size, const uint8_t nonce_prefix[UTP_CRYPTO_NONCE_PREFIX_SIZE]);
utp_internal_error_t utp_crypto_aead_seal(const utp_crypto_aead_t* aead, uint64_t packet_number,
                                          const uint8_t* plaintext, size_t plaintext_length, const uint8_t* aad,
                                          size_t aad_length, uint8_t* ciphertext, size_t ciphertext_capacity,
                                          size_t* ciphertext_length);
utp_internal_error_t utp_crypto_aead_open(const utp_crypto_aead_t* aead, uint64_t packet_number,
                                          const uint8_t* ciphertext, size_t ciphertext_length, const uint8_t* aad,
                                          size_t aad_length, uint8_t* plaintext, size_t plaintext_capacity,
                                          size_t* plaintext_length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_CRYPTO_H
