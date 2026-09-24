#include "crypto/crypto.h"

#include <limits.h>
#include <string.h>

#include <openssl/aead.h>
#include <openssl/curve25519.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "proto/proto.h"

#define UTP_CRYPTO_TRANSCRIPT_SIZE (4u + 4u + 4u + 1u + UTP_CRYPTO_X25519_KEY_SIZE + UTP_CRYPTO_X25519_KEY_SIZE)
#define UTP_CRYPTO_EXPANDED_SIZE   (2u * (UTP_CRYPTO_MAX_KEY_SIZE + UTP_CRYPTO_NONCE_PREFIX_SIZE))

static const uint8_t k_salt_label[]                                           = "libutp-handshake-v2";
static const uint8_t k_info_label[]                                           = "libutp-traffic-keys-v2";
static const uint8_t k_default_resumption_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE] = {
    0x75u, 0x74u, 0x70u, 0x2du, 0x64u, 0x65u, 0x66u, 0x61u, 0x75u, 0x6cu, 0x74u, 0x2du, 0x72u, 0x65u, 0x73u, 0x75u,
    0x6du, 0x70u, 0x74u, 0x69u, 0x6fu, 0x6eu, 0x2du, 0x6bu, 0x65u, 0x79u, 0x2du, 0x76u, 0x31u, 0x00u, 0x01u, 0x5au,
};
static const uint8_t k_ticket_seal_info[]             = "libutp-ticket-seal-key-v1";
static const uint8_t k_local_state_info[]             = "libutp-local-state-key-v1";
static const uint8_t k_early_salt_label[]             = "libutp-0rtt-early-v1";
static const uint8_t k_early_c2s_info[]               = "libutp-0rtt-c2s-v1";
static const uint8_t k_early_s2c_info[]               = "libutp-0rtt-s2c-v1";
static const uint8_t k_session_token_aad[]            = "UTP-SessionToken";
static const uint8_t k_local_resumption_state_aad[]   = "UTP-LocalResumptionState";
static const uint8_t k_local_resumption_state_magic[] = {'U', 'R', 'S', '1'};

static bool          is_all_zero(const uint8_t* data, size_t length)
{
    uint8_t aggregate = 0;
    size_t  index;

    for (index = 0; index < length; ++index) {
        aggregate |= data[index];
    }
    return aggregate == 0;
}

static void store_be32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static void store_be64(uint8_t output[8], uint64_t value)
{
    output[0] = (uint8_t)(value >> 56u);
    output[1] = (uint8_t)(value >> 48u);
    output[2] = (uint8_t)(value >> 40u);
    output[3] = (uint8_t)(value >> 32u);
    output[4] = (uint8_t)(value >> 24u);
    output[5] = (uint8_t)(value >> 16u);
    output[6] = (uint8_t)(value >> 8u);
    output[7] = (uint8_t)value;
}

static uint64_t load_be64(const uint8_t input[8])
{
    return ((uint64_t)input[0] << 56u) | ((uint64_t)input[1] << 48u) | ((uint64_t)input[2] << 40u) |
           ((uint64_t)input[3] << 32u) | ((uint64_t)input[4] << 24u) | ((uint64_t)input[5] << 16u) |
           ((uint64_t)input[6] << 8u) | input[7];
}

static void build_session_token_aad(uint8_t aad[sizeof(k_session_token_aad) - 1u + 8u], uint64_t expires_at_seconds)
{
    memcpy(aad, k_session_token_aad, sizeof(k_session_token_aad) - 1u);
    store_be64(aad + sizeof(k_session_token_aad) - 1u, expires_at_seconds);
}

static const EVP_AEAD* aead_for_key_size(size_t key_size)
{
    if (key_size == UTP_CRYPTO_AES_128_KEY_SIZE) {
        return EVP_aead_aes_128_gcm();
    }
    if (key_size == UTP_CRYPTO_AES_256_KEY_SIZE) {
        return EVP_aead_aes_256_gcm();
    }
    return NULL;
}

static void build_nonce(uint8_t nonce[UTP_CRYPTO_AEAD_NONCE_SIZE], const uint8_t prefix[UTP_CRYPTO_NONCE_PREFIX_SIZE],
                        uint64_t packet_number)
{
    memcpy(nonce, prefix, UTP_CRYPTO_NONCE_PREFIX_SIZE);
    store_be64(nonce + UTP_CRYPTO_NONCE_PREFIX_SIZE, packet_number);
}

void utp_crypto_secure_clear(void* data, size_t length)
{
    if (data != NULL && length != 0u) {
        OPENSSL_cleanse(data, length);
    }
}

void utp_crypto_key_pair_clear(utp_crypto_key_pair_t* key_pair)
{
    if (key_pair != NULL) {
        OPENSSL_cleanse(key_pair, sizeof(*key_pair));
    }
}

utp_internal_error_t utp_crypto_key_pair_generate(utp_crypto_key_pair_t* key_pair)
{
    if (key_pair == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }

    X25519_keypair(key_pair->public_key, key_pair->private_key);
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_key_pair_from_private(utp_crypto_key_pair_t* key_pair,
                                                      const uint8_t          private_key[UTP_CRYPTO_X25519_KEY_SIZE])
{
    if (key_pair == NULL || private_key == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }

    memcpy(key_pair->private_key, private_key, sizeof(key_pair->private_key));
    X25519_public_from_private(key_pair->public_key, key_pair->private_key);
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_x25519_derive(uint8_t       shared_secret[UTP_CRYPTO_X25519_KEY_SIZE],
                                              const uint8_t private_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                              const uint8_t peer_public_key[UTP_CRYPTO_X25519_KEY_SIZE])
{
    if (shared_secret == NULL || private_key == NULL || peer_public_key == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (X25519(shared_secret, private_key, peer_public_key) != 1) {
        OPENSSL_cleanse(shared_secret, UTP_CRYPTO_X25519_KEY_SIZE);
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    if (is_all_zero(shared_secret, UTP_CRYPTO_X25519_KEY_SIZE)) {
        OPENSSL_cleanse(shared_secret, UTP_CRYPTO_X25519_KEY_SIZE);
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    return UTP_INTERNAL_ERROR_OK;
}

void utp_crypto_traffic_material_clear(utp_crypto_traffic_material_t* material)
{
    if (material != NULL) {
        OPENSSL_cleanse(material, sizeof(*material));
    }
}

utp_internal_error_t utp_crypto_derive_traffic_material(utp_crypto_traffic_material_t* material,
                                                        const uint8_t shared_secret[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        const uint8_t client_public_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        const uint8_t server_public_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        uint32_t client_cid, uint32_t server_cid, uint8_t crypto_type,
                                                        size_t key_size)
{
    uint8_t transcript[UTP_CRYPTO_TRANSCRIPT_SIZE];
    uint8_t transcript_hash[UTP_CRYPTO_SHA256_SIZE];
    uint8_t salt_input[sizeof(k_salt_label) - 1u + UTP_CRYPTO_SHA256_SIZE];
    uint8_t salt[UTP_CRYPTO_SHA256_SIZE];
    uint8_t info[sizeof(k_info_label) - 1u + UTP_CRYPTO_SHA256_SIZE];
    uint8_t secret[UTP_CRYPTO_SHA256_SIZE];
    uint8_t expanded[UTP_CRYPTO_EXPANDED_SIZE];
    size_t  offset = 0;
    size_t  extracted_size;
    size_t  secret_size;
    size_t  output_size;

    if (material == NULL || shared_secret == NULL || client_public_key == NULL || server_public_key == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_crypto_traffic_material_clear(material);
    if ((key_size != UTP_CRYPTO_AES_128_KEY_SIZE && key_size != UTP_CRYPTO_AES_256_KEY_SIZE) || client_cid == 0 ||
        server_cid == 0 || is_all_zero(shared_secret, UTP_CRYPTO_X25519_KEY_SIZE)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }

    store_be32(transcript + offset, UTP_PROTOCOL_VERSION);
    offset += 4u;
    store_be32(transcript + offset, client_cid);
    offset += 4u;
    store_be32(transcript + offset, server_cid);
    offset               += 4u;
    transcript[offset++]  = crypto_type;
    memcpy(transcript + offset, client_public_key, UTP_CRYPTO_X25519_KEY_SIZE);
    offset += UTP_CRYPTO_X25519_KEY_SIZE;
    memcpy(transcript + offset, server_public_key, UTP_CRYPTO_X25519_KEY_SIZE);

    SHA256(transcript, sizeof(transcript), transcript_hash);
    memcpy(salt_input, k_salt_label, sizeof(k_salt_label) - 1u);
    memcpy(salt_input + sizeof(k_salt_label) - 1u, transcript_hash, sizeof(transcript_hash));
    SHA256(salt_input, sizeof(salt_input), salt);
    memcpy(info, k_info_label, sizeof(k_info_label) - 1u);
    memcpy(info + sizeof(k_info_label) - 1u, transcript_hash, sizeof(transcript_hash));

    secret_size    = key_size + UTP_CRYPTO_NONCE_PREFIX_SIZE;
    output_size    = 2u * secret_size;
    extracted_size = sizeof(secret);
    if (HKDF_extract(secret, &extracted_size, EVP_sha256(), shared_secret, UTP_CRYPTO_X25519_KEY_SIZE, salt,
                     sizeof(salt)) != 1 ||
        extracted_size != sizeof(secret) ||
        HKDF_expand(expanded, output_size, EVP_sha256(), secret, extracted_size, info, sizeof(info)) != 1) {
        OPENSSL_cleanse(transcript, sizeof(transcript));
        OPENSSL_cleanse(transcript_hash, sizeof(transcript_hash));
        OPENSSL_cleanse(salt_input, sizeof(salt_input));
        OPENSSL_cleanse(salt, sizeof(salt));
        OPENSSL_cleanse(info, sizeof(info));
        OPENSSL_cleanse(secret, sizeof(secret));
        OPENSSL_cleanse(expanded, sizeof(expanded));
        return UTP_INTERNAL_ERROR_CRYPTO;
    }

    memcpy(material->client_to_server.key, expanded, key_size);
    memcpy(material->client_to_server.nonce_prefix, expanded + key_size, UTP_CRYPTO_NONCE_PREFIX_SIZE);
    memcpy(material->server_to_client.key, expanded + secret_size, key_size);
    memcpy(material->server_to_client.nonce_prefix, expanded + secret_size + key_size, UTP_CRYPTO_NONCE_PREFIX_SIZE);
    material->key_size = key_size;

    OPENSSL_cleanse(transcript, sizeof(transcript));
    OPENSSL_cleanse(transcript_hash, sizeof(transcript_hash));
    OPENSSL_cleanse(salt_input, sizeof(salt_input));
    OPENSSL_cleanse(salt, sizeof(salt));
    OPENSSL_cleanse(info, sizeof(info));
    OPENSSL_cleanse(secret, sizeof(secret));
    OPENSSL_cleanse(expanded, sizeof(expanded));
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_create_directional_aead(const utp_crypto_key_pair_t* local_key_pair,
                                                        const uint8_t peer_public_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        uint32_t client_cid, uint32_t server_cid, uint8_t crypto_type,
                                                        bool local_is_client, utp_crypto_aead_t* tx,
                                                        utp_crypto_aead_t* rx)
{
    uint8_t                            shared_secret[UTP_CRYPTO_X25519_KEY_SIZE];
    utp_crypto_traffic_material_t      material;
    const uint8_t*                     client_public_key;
    const uint8_t*                     server_public_key;
    const utp_crypto_traffic_secret_t* tx_secret;
    const utp_crypto_traffic_secret_t* rx_secret;
    size_t                             key_size;
    utp_internal_error_t               error;

    if (local_key_pair == NULL || peer_public_key == NULL || tx == NULL || rx == NULL || client_cid == 0u ||
        server_cid == 0u || crypto_type > UTP_CRYPTO_TYPE_AES_GCM_256) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    key_size = crypto_type == UTP_CRYPTO_TYPE_AES_GCM_256 ? UTP_CRYPTO_AES_256_KEY_SIZE : UTP_CRYPTO_AES_128_KEY_SIZE;
    client_public_key = local_is_client ? local_key_pair->public_key : peer_public_key;
    server_public_key = local_is_client ? peer_public_key : local_key_pair->public_key;
    error             = utp_crypto_x25519_derive(shared_secret, local_key_pair->private_key, peer_public_key);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_crypto_derive_traffic_material(&material, shared_secret, client_public_key, server_public_key,
                                                   client_cid, server_cid, crypto_type, key_size);
    }
    OPENSSL_cleanse(shared_secret, sizeof(shared_secret));
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    tx_secret = local_is_client ? &material.client_to_server : &material.server_to_client;
    rx_secret = local_is_client ? &material.server_to_client : &material.client_to_server;
    error     = utp_crypto_aead_init(tx, NULL, tx_secret->key, key_size, tx_secret->nonce_prefix);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_crypto_aead_init(rx, NULL, rx_secret->key, key_size, rx_secret->nonce_prefix);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_crypto_aead_cleanup(tx);
        utp_crypto_aead_cleanup(rx);
    }
    utp_crypto_traffic_material_clear(&material);
    return error;
}

void utp_crypto_aead_cleanup(utp_crypto_aead_t* aead)
{
    EVP_AEAD_CTX*          provider_context;
    const utp_allocator_t* allocator;

    if (aead == NULL) {
        return;
    }
    provider_context = (EVP_AEAD_CTX*)aead->provider_context;
    allocator        = aead->allocator;
    if (provider_context != NULL) {
        EVP_AEAD_CTX_cleanup(provider_context);
        utp_allocator_free(allocator, provider_context);
    }
    OPENSSL_cleanse(aead, sizeof(*aead));
}

utp_internal_error_t utp_crypto_aead_init(utp_crypto_aead_t* aead, const utp_allocator_t* allocator, const uint8_t* key,
                                          size_t key_size, const uint8_t nonce_prefix[UTP_CRYPTO_NONCE_PREFIX_SIZE])
{
    const EVP_AEAD*        provider_aead;
    EVP_AEAD_CTX*          provider_context;
    const utp_allocator_t* resolved_allocator;

    if (aead == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_crypto_aead_cleanup(aead);
    if (key == NULL || nonce_prefix == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    provider_aead      = aead_for_key_size(key_size);
    resolved_allocator = utp_allocator_resolve(allocator);
    if (provider_aead == NULL || resolved_allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }

    provider_context = utp_allocator_alloc(resolved_allocator, sizeof(*provider_context));
    if (provider_context == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    EVP_AEAD_CTX_zero(provider_context);
    if (EVP_AEAD_CTX_init(provider_context, provider_aead, key, key_size, UTP_CRYPTO_AEAD_TAG_SIZE, NULL) != 1) {
        EVP_AEAD_CTX_cleanup(provider_context);
        utp_allocator_free(resolved_allocator, provider_context);
        return UTP_INTERNAL_ERROR_CRYPTO;
    }

    aead->provider_context = provider_context;
    aead->allocator        = resolved_allocator;
    memcpy(aead->nonce_prefix, nonce_prefix, sizeof(aead->nonce_prefix));
    aead->key_size = key_size;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_aead_seal(const utp_crypto_aead_t* aead, uint64_t packet_number,
                                          const uint8_t* plaintext, size_t plaintext_length, const uint8_t* aad,
                                          size_t aad_length, uint8_t* ciphertext, size_t ciphertext_capacity,
                                          size_t* ciphertext_length)
{
    uint8_t       nonce[UTP_CRYPTO_AEAD_NONCE_SIZE];
    EVP_AEAD_CTX* provider_context;
    size_t        required_length;
    size_t        output_length = 0;

    if (ciphertext_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *ciphertext_length = 0;
    if (aead == NULL || aead->provider_context == NULL || ciphertext == NULL ||
        (plaintext == NULL && plaintext_length != 0) || (aad == NULL && aad_length != 0)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (plaintext_length > SIZE_MAX - UTP_CRYPTO_AEAD_TAG_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    required_length = plaintext_length + UTP_CRYPTO_AEAD_TAG_SIZE;
    if (ciphertext_capacity < required_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }

    provider_context = (EVP_AEAD_CTX*)aead->provider_context;
    build_nonce(nonce, aead->nonce_prefix, packet_number);
    if (EVP_AEAD_CTX_seal(provider_context, ciphertext, &output_length, ciphertext_capacity, nonce, sizeof(nonce),
                          plaintext, plaintext_length, aad, aad_length) != 1) {
        OPENSSL_cleanse(nonce, sizeof(nonce));
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    OPENSSL_cleanse(nonce, sizeof(nonce));
    *ciphertext_length = output_length;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_aead_open(const utp_crypto_aead_t* aead, uint64_t packet_number,
                                          const uint8_t* ciphertext, size_t ciphertext_length, const uint8_t* aad,
                                          size_t aad_length, uint8_t* plaintext, size_t plaintext_capacity,
                                          size_t* plaintext_length)
{
    uint8_t       nonce[UTP_CRYPTO_AEAD_NONCE_SIZE];
    EVP_AEAD_CTX* provider_context;
    size_t        required_length;
    size_t        output_length = 0;

    if (plaintext_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *plaintext_length = 0;
    if (aead == NULL || aead->provider_context == NULL || ciphertext == NULL || plaintext == NULL ||
        (aad == NULL && aad_length != 0)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (ciphertext_length < UTP_CRYPTO_AEAD_TAG_SIZE) {
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    required_length = ciphertext_length - UTP_CRYPTO_AEAD_TAG_SIZE;
    if (plaintext_capacity < required_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }

    provider_context = (EVP_AEAD_CTX*)aead->provider_context;
    build_nonce(nonce, aead->nonce_prefix, packet_number);
    if (EVP_AEAD_CTX_open(provider_context, plaintext, &output_length, plaintext_capacity, nonce, sizeof(nonce),
                          ciphertext, ciphertext_length, aad, aad_length) != 1) {
        OPENSSL_cleanse(nonce, sizeof(nonce));
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    OPENSSL_cleanse(nonce, sizeof(nonce));
    *plaintext_length = output_length;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_crypto_default_resumption_key(uint8_t key[UTP_CRYPTO_RESUMPTION_KEY_SIZE])
{
    if (key != NULL) {
        memcpy(key, k_default_resumption_key, sizeof(k_default_resumption_key));
    }
}

void utp_crypto_resumption_keys_clear(utp_crypto_resumption_keys_t* keys)
{
    utp_crypto_secure_clear(keys, keys == NULL ? 0u : sizeof(*keys));
}

utp_internal_error_t utp_crypto_derive_resumption_keys(utp_crypto_resumption_keys_t* keys,
                                                       const uint8_t root_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE])
{
    utp_crypto_resumption_keys_t derived;
    uint8_t                      secret[UTP_CRYPTO_SHA256_SIZE];
    size_t                       secret_length = sizeof(secret);

    if (keys == NULL || root_key == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (HKDF_extract(secret, &secret_length, EVP_sha256(), root_key, UTP_CRYPTO_RESUMPTION_KEY_SIZE, NULL, 0u) != 1 ||
        secret_length != sizeof(secret) ||
        HKDF_expand(derived.ticket_seal_key, sizeof(derived.ticket_seal_key), EVP_sha256(), secret, secret_length,
                    k_ticket_seal_info, sizeof(k_ticket_seal_info) - 1u) != 1 ||
        HKDF_expand(derived.local_state_key, sizeof(derived.local_state_key), EVP_sha256(), secret, secret_length,
                    k_local_state_info, sizeof(k_local_state_info) - 1u) != 1) {
        utp_crypto_secure_clear(&derived, sizeof(derived));
        utp_crypto_secure_clear(secret, sizeof(secret));
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    memcpy(keys, &derived, sizeof(*keys));
    utp_crypto_secure_clear(&derived, sizeof(derived));
    utp_crypto_secure_clear(secret, sizeof(secret));
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_derive_early_aead(
    utp_crypto_aead_t* aead, const uint8_t resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE],
    const uint8_t early_attempt_nonce[UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE],
    const uint8_t encrypted_server_info[UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE], uint8_t crypto_type,
    bool client_to_server)
{
    uint8_t              salt[UTP_CRYPTO_SHA256_SIZE];
    uint8_t              secret[UTP_CRYPTO_SHA256_SIZE];
    uint8_t              info[sizeof(k_early_c2s_info) - 1u + UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE +
                 UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE + 1u];
    uint8_t              material[UTP_CRYPTO_MAX_KEY_SIZE + UTP_CRYPTO_NONCE_PREFIX_SIZE];
    const uint8_t*       label;
    size_t               label_length;
    size_t               key_size;
    size_t               material_length;
    size_t               secret_length;
    size_t               offset;
    utp_internal_error_t error;

    if (aead == NULL || resumption_psk == NULL || early_attempt_nonce == NULL || encrypted_server_info == NULL ||
        crypto_type > UTP_CRYPTO_TYPE_AES_GCM_256) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    key_size = crypto_type == UTP_CRYPTO_TYPE_AES_GCM_256 ? UTP_CRYPTO_AES_256_KEY_SIZE : UTP_CRYPTO_AES_128_KEY_SIZE;
    material_length = key_size + UTP_CRYPTO_NONCE_PREFIX_SIZE;
    label           = client_to_server ? k_early_c2s_info : k_early_s2c_info;
    label_length    = client_to_server ? sizeof(k_early_c2s_info) - 1u : sizeof(k_early_s2c_info) - 1u;
    SHA256(k_early_salt_label, sizeof(k_early_salt_label) - 1u, salt);
    secret_length = sizeof(secret);
    if (HKDF_extract(secret, &secret_length, EVP_sha256(), resumption_psk, UTP_CRYPTO_RESUMPTION_PSK_SIZE, salt,
                     sizeof(salt)) != 1 ||
        secret_length != UTP_CRYPTO_SHA256_SIZE) {
        utp_crypto_secure_clear(salt, sizeof(salt));
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    memcpy(info, label, label_length);
    offset = label_length;
    memcpy(info + offset, early_attempt_nonce, UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE);
    offset += UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE;
    memcpy(info + offset, encrypted_server_info, UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE);
    offset       += UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE;
    info[offset]  = crypto_type == UTP_CRYPTO_TYPE_AES_GCM_256 ? UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_256
                                                               : UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_128;
    if (HKDF_expand(material, material_length, EVP_sha256(), secret, secret_length, info, offset + 1u) != 1) {
        utp_crypto_secure_clear(salt, sizeof(salt));
        utp_crypto_secure_clear(secret, sizeof(secret));
        utp_crypto_secure_clear(material, sizeof(material));
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    error = utp_crypto_aead_init(aead, NULL, material, key_size, material + key_size);
    utp_crypto_secure_clear(salt, sizeof(salt));
    utp_crypto_secure_clear(secret, sizeof(secret));
    utp_crypto_secure_clear(material, sizeof(material));
    return error;
}

utp_internal_error_t utp_crypto_aes256gcm_seal(const uint8_t  key[UTP_CRYPTO_RESUMPTION_KEY_SIZE],
                                               const uint8_t* plaintext, size_t plaintext_length, const uint8_t* aad,
                                               size_t aad_length, uint8_t* sealed, size_t sealed_capacity,
                                               size_t* sealed_length)
{
    EVP_AEAD_CTX context;
    size_t       ciphertext_length = 0u;
    size_t       required_length;

    if (sealed_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *sealed_length = 0u;
    if (key == NULL || (plaintext == NULL && plaintext_length != 0u) || (aad == NULL && aad_length != 0u) ||
        sealed == NULL || plaintext_length > SIZE_MAX - UTP_CRYPTO_AEAD_NONCE_SIZE - UTP_CRYPTO_AEAD_TAG_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    required_length = UTP_CRYPTO_AEAD_NONCE_SIZE + plaintext_length + UTP_CRYPTO_AEAD_TAG_SIZE;
    if (sealed_capacity < required_length || RAND_bytes(sealed, UTP_CRYPTO_AEAD_NONCE_SIZE) != 1) {
        return sealed_capacity < required_length ? UTP_INTERNAL_ERROR_OVERFLOW : UTP_INTERNAL_ERROR_CRYPTO;
    }
    EVP_AEAD_CTX_zero(&context);
    if (EVP_AEAD_CTX_init(&context, EVP_aead_aes_256_gcm(), key, UTP_CRYPTO_RESUMPTION_KEY_SIZE,
                          UTP_CRYPTO_AEAD_TAG_SIZE, NULL) != 1 ||
        EVP_AEAD_CTX_seal(&context, sealed + UTP_CRYPTO_AEAD_NONCE_SIZE, &ciphertext_length,
                          sealed_capacity - UTP_CRYPTO_AEAD_NONCE_SIZE, sealed, UTP_CRYPTO_AEAD_NONCE_SIZE, plaintext,
                          plaintext_length, aad, aad_length) != 1) {
        EVP_AEAD_CTX_cleanup(&context);
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    EVP_AEAD_CTX_cleanup(&context);
    *sealed_length = UTP_CRYPTO_AEAD_NONCE_SIZE + ciphertext_length;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_aes256gcm_open(const uint8_t key[UTP_CRYPTO_RESUMPTION_KEY_SIZE], const uint8_t* sealed,
                                               size_t sealed_length, const uint8_t* aad, size_t aad_length,
                                               uint8_t* plaintext, size_t plaintext_capacity, size_t* plaintext_length)
{
    EVP_AEAD_CTX context;
    size_t       output_length = 0u;
    size_t       required_length;

    if (plaintext_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *plaintext_length = 0u;
    if (key == NULL || sealed == NULL || plaintext == NULL || (aad == NULL && aad_length != 0u) ||
        sealed_length < UTP_CRYPTO_AEAD_NONCE_SIZE + UTP_CRYPTO_AEAD_TAG_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    required_length = sealed_length - UTP_CRYPTO_AEAD_NONCE_SIZE - UTP_CRYPTO_AEAD_TAG_SIZE;
    if (plaintext_capacity < required_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    EVP_AEAD_CTX_zero(&context);
    if (EVP_AEAD_CTX_init(&context, EVP_aead_aes_256_gcm(), key, UTP_CRYPTO_RESUMPTION_KEY_SIZE,
                          UTP_CRYPTO_AEAD_TAG_SIZE, NULL) != 1 ||
        EVP_AEAD_CTX_open(&context, plaintext, &output_length, plaintext_capacity, sealed, UTP_CRYPTO_AEAD_NONCE_SIZE,
                          sealed + UTP_CRYPTO_AEAD_NONCE_SIZE, sealed_length - UTP_CRYPTO_AEAD_NONCE_SIZE, aad,
                          aad_length) != 1) {
        EVP_AEAD_CTX_cleanup(&context);
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    EVP_AEAD_CTX_cleanup(&context);
    *plaintext_length = output_length;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_random_bytes(uint8_t* data, size_t length)
{
    if (data == NULL || length == 0u || length > (size_t)INT_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return RAND_bytes(data, length) == 1 ? UTP_INTERNAL_ERROR_OK : UTP_INTERNAL_ERROR_CRYPTO;
}

utp_internal_error_t utp_crypto_sha256(const uint8_t* data, size_t length, uint8_t digest[UTP_CRYPTO_SHA256_SIZE])
{
    if ((data == NULL && length != 0u) || digest == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return SHA256(data, length, digest) == NULL ? UTP_INTERNAL_ERROR_CRYPTO : UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_server_info_seal(const uint8_t ticket_seal_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE],
                                                 const uint8_t resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE],
                                                 uint8_t encryption_mode, uint64_t expires_at_seconds,
                                                 uint8_t encrypted_server_info[UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE])
{
    uint8_t              plaintext[UTP_CRYPTO_SERVER_INFO_PLAINTEXT_SIZE];
    uint8_t              aad[sizeof(k_session_token_aad) - 1u + 8u];
    size_t               sealed_length = 0u;
    utp_internal_error_t error;

    if (ticket_seal_key == NULL || resumption_psk == NULL || encrypted_server_info == NULL ||
        encryption_mode > UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_256) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memcpy(plaintext, resumption_psk, UTP_CRYPTO_RESUMPTION_PSK_SIZE);
    plaintext[UTP_CRYPTO_RESUMPTION_PSK_SIZE] = encryption_mode;
    for (size_t index = UTP_CRYPTO_RESUMPTION_PSK_SIZE + 1u; index < sizeof(plaintext); ++index) {
        plaintext[index] = 0u;
    }
    build_session_token_aad(aad, expires_at_seconds);
    error = utp_crypto_aes256gcm_seal(ticket_seal_key, plaintext, sizeof(plaintext), aad, sizeof(aad),
                                      encrypted_server_info, UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE, &sealed_length);
    utp_crypto_secure_clear(plaintext, sizeof(plaintext));
    utp_crypto_secure_clear(aad, sizeof(aad));
    return error == UTP_INTERNAL_ERROR_OK && sealed_length != UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE
               ? UTP_INTERNAL_ERROR_CRYPTO
               : error;
}

utp_internal_error_t utp_crypto_server_info_open(
    const uint8_t ticket_seal_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE], uint64_t expires_at_seconds,
    const uint8_t encrypted_server_info[UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE],
    uint8_t resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE], uint8_t* encryption_mode)
{
    uint8_t              plaintext[UTP_CRYPTO_SERVER_INFO_PLAINTEXT_SIZE];
    uint8_t              aad[sizeof(k_session_token_aad) - 1u + 8u];
    size_t               plaintext_length = 0u;
    utp_internal_error_t error;

    if (ticket_seal_key == NULL || encrypted_server_info == NULL || resumption_psk == NULL || encryption_mode == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    build_session_token_aad(aad, expires_at_seconds);
    error = utp_crypto_aes256gcm_open(ticket_seal_key, encrypted_server_info, UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE,
                                      aad, sizeof(aad), plaintext, sizeof(plaintext), &plaintext_length);
    utp_crypto_secure_clear(aad, sizeof(aad));
    if (error != UTP_INTERNAL_ERROR_OK || plaintext_length != sizeof(plaintext) ||
        plaintext[UTP_CRYPTO_RESUMPTION_PSK_SIZE] > UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_256) {
        utp_crypto_secure_clear(plaintext, sizeof(plaintext));
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_CRYPTO : error;
    }
    memcpy(resumption_psk, plaintext, UTP_CRYPTO_RESUMPTION_PSK_SIZE);
    *encryption_mode = plaintext[UTP_CRYPTO_RESUMPTION_PSK_SIZE];
    utp_crypto_secure_clear(plaintext, sizeof(plaintext));
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_local_resumption_state_seal(
    const uint8_t local_state_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE], uint8_t encryption_mode, uint64_t expires_at_seconds,
    const uint8_t* payload, uint8_t payload_length, uint8_t* state, size_t state_capacity, size_t* state_length)
{
    uint8_t              plaintext[UTP_CRYPTO_LOCAL_RESUMPTION_FIXED_PLAINTEXT_SIZE + UINT8_MAX];
    size_t               plaintext_length;
    size_t               sealed_length = 0u;
    utp_internal_error_t error;

    if (state_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *state_length = 0u;
    if (local_state_key == NULL || state == NULL || (payload == NULL && payload_length != 0u) ||
        encryption_mode > UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_256) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (state_capacity < UTP_CRYPTO_LOCAL_RESUMPTION_OVERHEAD_SIZE + payload_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    plaintext[0] = encryption_mode;
    store_be64(plaintext + 1u, expires_at_seconds);
    if (payload_length != 0u) {
        memcpy(plaintext + UTP_CRYPTO_LOCAL_RESUMPTION_FIXED_PLAINTEXT_SIZE, payload, payload_length);
    }
    plaintext_length = UTP_CRYPTO_LOCAL_RESUMPTION_FIXED_PLAINTEXT_SIZE + payload_length;
    memcpy(state, k_local_resumption_state_magic, sizeof(k_local_resumption_state_magic));
    error = utp_crypto_aes256gcm_seal(local_state_key, plaintext, plaintext_length, k_local_resumption_state_aad,
                                      sizeof(k_local_resumption_state_aad) - 1u,
                                      state + sizeof(k_local_resumption_state_magic),
                                      state_capacity - sizeof(k_local_resumption_state_magic), &sealed_length);
    utp_crypto_secure_clear(plaintext, plaintext_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        *state_length = sizeof(k_local_resumption_state_magic) + sealed_length;
    }
    return error;
}

utp_internal_error_t utp_crypto_local_resumption_state_open(
    const uint8_t local_state_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE], const uint8_t* state, size_t state_length,
    uint8_t* encryption_mode, uint64_t* expires_at_seconds, uint8_t* payload, size_t payload_capacity,
    size_t* payload_length)
{
    uint8_t              plaintext[UTP_CRYPTO_LOCAL_RESUMPTION_FIXED_PLAINTEXT_SIZE + UINT8_MAX];
    size_t               plaintext_length = 0u;
    size_t               decoded_payload_length;
    utp_internal_error_t error;

    if (payload_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *payload_length = 0u;
    if (local_state_key == NULL || state == NULL || encryption_mode == NULL || expires_at_seconds == NULL ||
        payload == NULL || state_length < UTP_CRYPTO_LOCAL_RESUMPTION_OVERHEAD_SIZE ||
        state_length > UTP_CRYPTO_LOCAL_RESUMPTION_STATE_MAX_SIZE ||
        memcmp(state, k_local_resumption_state_magic, sizeof(k_local_resumption_state_magic)) != 0) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_crypto_aes256gcm_open(local_state_key, state + sizeof(k_local_resumption_state_magic),
                                      state_length - sizeof(k_local_resumption_state_magic),
                                      k_local_resumption_state_aad, sizeof(k_local_resumption_state_aad) - 1u,
                                      plaintext, sizeof(plaintext), &plaintext_length);
    if (error != UTP_INTERNAL_ERROR_OK || plaintext_length < UTP_CRYPTO_LOCAL_RESUMPTION_FIXED_PLAINTEXT_SIZE ||
        plaintext[0] > UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_256) {
        utp_crypto_secure_clear(plaintext, sizeof(plaintext));
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_CRYPTO : error;
    }
    decoded_payload_length = plaintext_length - UTP_CRYPTO_LOCAL_RESUMPTION_FIXED_PLAINTEXT_SIZE;
    if (payload_capacity < decoded_payload_length) {
        utp_crypto_secure_clear(plaintext, plaintext_length);
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    *encryption_mode    = plaintext[0];
    *expires_at_seconds = load_be64(plaintext + 1u);
    memcpy(payload, plaintext + UTP_CRYPTO_LOCAL_RESUMPTION_FIXED_PLAINTEXT_SIZE, decoded_payload_length);
    *payload_length = decoded_payload_length;
    utp_crypto_secure_clear(plaintext, plaintext_length);
    return UTP_INTERNAL_ERROR_OK;
}
