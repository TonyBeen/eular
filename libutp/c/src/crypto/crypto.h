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

#define UTP_CRYPTO_X25519_KEY_SIZE                       32u
#define UTP_CRYPTO_AES_128_KEY_SIZE                      16u
#define UTP_CRYPTO_AES_256_KEY_SIZE                      32u
#define UTP_CRYPTO_MAX_KEY_SIZE                          UTP_CRYPTO_AES_256_KEY_SIZE
#define UTP_CRYPTO_NONCE_PREFIX_SIZE                     4u
#define UTP_CRYPTO_AEAD_NONCE_SIZE                       12u
#define UTP_CRYPTO_AEAD_TAG_SIZE                         16u
#define UTP_CRYPTO_SHA256_SIZE                           32u
#define UTP_CRYPTO_RESUMPTION_KEY_SIZE                   32u
#define UTP_CRYPTO_RESUMPTION_PSK_SIZE                   32u
#define UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE              16u
#define UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE            93u
#define UTP_CRYPTO_SERVER_INFO_PLAINTEXT_SIZE            65u
#define UTP_CRYPTO_SESSION_TOKEN_PAYLOAD_SIZE            (UTP_CRYPTO_RESUMPTION_PSK_SIZE + UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE)
#define UTP_CRYPTO_LOCAL_RESUMPTION_FIXED_PLAINTEXT_SIZE 9u
#define UTP_CRYPTO_LOCAL_RESUMPTION_OVERHEAD_SIZE \
    (4u + UTP_CRYPTO_AEAD_NONCE_SIZE + UTP_CRYPTO_LOCAL_RESUMPTION_FIXED_PLAINTEXT_SIZE + UTP_CRYPTO_AEAD_TAG_SIZE)
#define UTP_CRYPTO_LOCAL_RESUMPTION_STATE_SIZE \
    (UTP_CRYPTO_LOCAL_RESUMPTION_OVERHEAD_SIZE + UTP_CRYPTO_SESSION_TOKEN_PAYLOAD_SIZE)
#define UTP_CRYPTO_LOCAL_RESUMPTION_STATE_MAX_SIZE (UTP_CRYPTO_LOCAL_RESUMPTION_OVERHEAD_SIZE + UINT8_MAX)

#define UTP_CRYPTO_TYPE_AES_GCM_128            0u
#define UTP_CRYPTO_TYPE_AES_GCM_256            1u
#define UTP_CRYPTO_ENCRYPTION_MODE_NONE        0u
#define UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_128 1u
#define UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_256 2u

typedef struct utp_crypto_key_pair {
    uint8_t public_key[UTP_CRYPTO_X25519_KEY_SIZE];   // X25519 公钥
    uint8_t private_key[UTP_CRYPTO_X25519_KEY_SIZE];  // X25519 私钥
} utp_crypto_key_pair_t;

typedef struct utp_crypto_traffic_secret {
    uint8_t key[UTP_CRYPTO_MAX_KEY_SIZE];                // AEAD 密钥材料
    uint8_t nonce_prefix[UTP_CRYPTO_NONCE_PREFIX_SIZE];  // 每包 nonce 固定前缀
} utp_crypto_traffic_secret_t;

typedef struct utp_crypto_traffic_material {
    utp_crypto_traffic_secret_t client_to_server;  // 客户端到服务端方向密钥
    utp_crypto_traffic_secret_t server_to_client;  // 服务端到客户端方向密钥
    size_t                      key_size;          // 实际使用的 AEAD 密钥长度
} utp_crypto_traffic_material_t;

typedef struct utp_crypto_aead {
    void*                  provider_context;                            // OpenSSL AEAD 上下文所有权
    const utp_allocator_t* allocator;                                   // 分配器，不拥有
    uint8_t                nonce_prefix[UTP_CRYPTO_NONCE_PREFIX_SIZE];  // nonce 固定前缀
    size_t                 key_size;                                    // AEAD 密钥长度
} utp_crypto_aead_t;

/** @brief 恢复根密钥派生出的服务端票据和本地状态工作密钥。 */
typedef struct utp_crypto_resumption_keys {
    uint8_t ticket_seal_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE];  // 服务端票据加密密钥
    uint8_t local_state_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE];  // 客户端恢复状态加密密钥
} utp_crypto_resumption_keys_t;

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
/** @brief 使用内置的固定恢复根密钥；仅用于未显式配置的兼容路径。 */
void                 utp_crypto_default_resumption_key(uint8_t key[UTP_CRYPTO_RESUMPTION_KEY_SIZE]);
/** @brief 从恢复根密钥分离派生票据及本地状态的 AES-256-GCM 工作密钥。 */
utp_internal_error_t utp_crypto_derive_resumption_keys(utp_crypto_resumption_keys_t* keys,
                                                       const uint8_t root_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE]);
/** @brief 清零恢复工作密钥。 */
void                 utp_crypto_resumption_keys_clear(utp_crypto_resumption_keys_t* keys);
/** @brief 从恢复 PSK 派生一条 early-data 单向 AEAD。 */
utp_internal_error_t utp_crypto_derive_early_aead(
    utp_crypto_aead_t* aead, const uint8_t resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE],
    const uint8_t early_attempt_nonce[UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE],
    const uint8_t encrypted_server_info[UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE], uint8_t crypto_type,
    bool client_to_server);
/** @brief 以随机 nonce 封装 AES-256-GCM 数据，输出为 nonce | ciphertext | tag。 */
utp_internal_error_t utp_crypto_aes256gcm_seal(const uint8_t  key[UTP_CRYPTO_RESUMPTION_KEY_SIZE],
                                               const uint8_t* plaintext, size_t plaintext_length, const uint8_t* aad,
                                               size_t aad_length, uint8_t* sealed, size_t sealed_capacity,
                                               size_t* sealed_length);
/** @brief 解开 nonce | ciphertext | tag 格式的 AES-256-GCM 数据。 */
utp_internal_error_t utp_crypto_aes256gcm_open(const uint8_t key[UTP_CRYPTO_RESUMPTION_KEY_SIZE], const uint8_t* sealed,
                                               size_t sealed_length, const uint8_t* aad, size_t aad_length,
                                               uint8_t* plaintext, size_t plaintext_capacity, size_t* plaintext_length);
/** @brief 清零任意敏感字节区。 */
void                 utp_crypto_secure_clear(void* data, size_t length);
/** @brief 生成加密强度随机字节。 */
utp_internal_error_t utp_crypto_random_bytes(uint8_t* data, size_t length);
/** @brief 计算 SHA-256 摘要。 */
utp_internal_error_t utp_crypto_sha256(const uint8_t* data, size_t length, uint8_t digest[UTP_CRYPTO_SHA256_SIZE]);
/** @brief 封装服务端专用恢复信息，输出固定 93 字节。 */
utp_internal_error_t utp_crypto_server_info_seal(const uint8_t ticket_seal_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE],
                                                 const uint8_t resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE],
                                                 uint8_t encryption_mode, uint64_t expires_at_seconds,
                                                 uint8_t encrypted_server_info[UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE]);
/** @brief 解开并校验服务端专用恢复信息。 */
utp_internal_error_t utp_crypto_server_info_open(
    const uint8_t ticket_seal_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE], uint64_t expires_at_seconds,
    const uint8_t encrypted_server_info[UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE],
    uint8_t resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE], uint8_t* encryption_mode);
/** @brief 封装客户端持有的恢复状态，当前 payload 编码生成 166 字节。 */
utp_internal_error_t utp_crypto_local_resumption_state_seal(
    const uint8_t local_state_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE], uint8_t encryption_mode, uint64_t expires_at_seconds,
    const uint8_t* payload, uint8_t payload_length, uint8_t* state, size_t state_capacity, size_t* state_length);
/** @brief 解开客户端持有的恢复状态；payload 长度由认证后的密文长度决定。 */
utp_internal_error_t utp_crypto_local_resumption_state_open(
    const uint8_t local_state_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE], const uint8_t* state, size_t state_length,
    uint8_t* encryption_mode, uint64_t* expires_at_seconds, uint8_t* payload, size_t payload_capacity,
    size_t* payload_length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_CRYPTO_H
