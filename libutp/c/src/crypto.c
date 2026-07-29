#include "internal/crypto.h"

#include <openssl/aead.h>
#include <openssl/curve25519.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <openssl/mem.h>
#include <openssl/sha.h>
#include <string.h>

#include "internal/proto.h"

#define UTP_CRYPTO_TRANSCRIPT_SIZE (4u + 4u + 4u + 1u + UTP_CRYPTO_X25519_KEY_SIZE + UTP_CRYPTO_X25519_KEY_SIZE)
#define UTP_CRYPTO_SHA256_SIZE     SHA256_DIGEST_LENGTH
#define UTP_CRYPTO_EXPANDED_SIZE   (2u * (UTP_CRYPTO_MAX_KEY_SIZE + UTP_CRYPTO_NONCE_PREFIX_SIZE))

static const uint8_t k_salt_label[] = "libutp-handshake-v2";
static const uint8_t k_info_label[] = "libutp-traffic-keys-v2";

static bool is_all_zero(const uint8_t *data, size_t length) {
    uint8_t aggregate = 0;
    size_t  index;

    for (index = 0; index < length; ++index) {
        aggregate |= data[index];
    }
    return aggregate == 0;
}

static void store_be32(uint8_t output[4], uint32_t value) {
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static void store_be64(uint8_t output[8], uint64_t value) {
    output[0] = (uint8_t)(value >> 56u);
    output[1] = (uint8_t)(value >> 48u);
    output[2] = (uint8_t)(value >> 40u);
    output[3] = (uint8_t)(value >> 32u);
    output[4] = (uint8_t)(value >> 24u);
    output[5] = (uint8_t)(value >> 16u);
    output[6] = (uint8_t)(value >> 8u);
    output[7] = (uint8_t)value;
}

static const EVP_AEAD *aead_for_key_size(size_t key_size) {
    if (key_size == UTP_CRYPTO_AES_128_KEY_SIZE) {
        return EVP_aead_aes_128_gcm();
    }
    if (key_size == UTP_CRYPTO_AES_256_KEY_SIZE) {
        return EVP_aead_aes_256_gcm();
    }
    return NULL;
}

static void build_nonce(uint8_t nonce[UTP_CRYPTO_AEAD_NONCE_SIZE], const uint8_t prefix[UTP_CRYPTO_NONCE_PREFIX_SIZE],
                        uint64_t packet_number) {
    memcpy(nonce, prefix, UTP_CRYPTO_NONCE_PREFIX_SIZE);
    store_be64(nonce + UTP_CRYPTO_NONCE_PREFIX_SIZE, packet_number);
}

void utp_crypto_key_pair_clear(utp_crypto_key_pair_t *key_pair) {
    if (key_pair != NULL) {
        OPENSSL_cleanse(key_pair, sizeof(*key_pair));
    }
}

utp_internal_error_t utp_crypto_key_pair_generate(utp_crypto_key_pair_t *key_pair) {
    if (key_pair == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }

    X25519_keypair(key_pair->public_key, key_pair->private_key);
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_key_pair_from_private(utp_crypto_key_pair_t *key_pair,
                                                      const uint8_t          private_key[UTP_CRYPTO_X25519_KEY_SIZE]) {
    if (key_pair == NULL || private_key == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }

    memcpy(key_pair->private_key, private_key, sizeof(key_pair->private_key));
    X25519_public_from_private(key_pair->public_key, key_pair->private_key);
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_crypto_x25519_derive(uint8_t       shared_secret[UTP_CRYPTO_X25519_KEY_SIZE],
                                              const uint8_t private_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                              const uint8_t peer_public_key[UTP_CRYPTO_X25519_KEY_SIZE]) {
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

void utp_crypto_traffic_material_clear(utp_crypto_traffic_material_t *material) {
    if (material != NULL) {
        OPENSSL_cleanse(material, sizeof(*material));
    }
}

utp_internal_error_t utp_crypto_derive_traffic_material(utp_crypto_traffic_material_t *material,
                                                        const uint8_t shared_secret[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        const uint8_t client_public_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        const uint8_t server_public_key[UTP_CRYPTO_X25519_KEY_SIZE],
                                                        uint32_t client_cid, uint32_t server_cid, uint8_t crypto_type,
                                                        size_t key_size) {
    uint8_t transcript[UTP_CRYPTO_TRANSCRIPT_SIZE];
    uint8_t transcript_hash[UTP_CRYPTO_SHA256_SIZE];
    uint8_t salt_input[sizeof(k_salt_label) - 1u + UTP_CRYPTO_SHA256_SIZE];
    uint8_t salt[UTP_CRYPTO_SHA256_SIZE];
    uint8_t info[sizeof(k_info_label) - 1u + UTP_CRYPTO_SHA256_SIZE];
    uint8_t expanded[UTP_CRYPTO_EXPANDED_SIZE];
    size_t  offset = 0;
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

    secret_size = key_size + UTP_CRYPTO_NONCE_PREFIX_SIZE;
    output_size = 2u * secret_size;
    if (HKDF(expanded, output_size, EVP_sha256(), shared_secret, UTP_CRYPTO_X25519_KEY_SIZE, salt, sizeof(salt), info,
             sizeof(info)) != 1) {
        OPENSSL_cleanse(transcript, sizeof(transcript));
        OPENSSL_cleanse(transcript_hash, sizeof(transcript_hash));
        OPENSSL_cleanse(salt_input, sizeof(salt_input));
        OPENSSL_cleanse(salt, sizeof(salt));
        OPENSSL_cleanse(info, sizeof(info));
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
    OPENSSL_cleanse(expanded, sizeof(expanded));
    return UTP_INTERNAL_ERROR_OK;
}

void utp_crypto_aead_cleanup(utp_crypto_aead_t *aead) {
    EVP_AEAD_CTX          *provider_context;
    const utp_allocator_t *allocator;

    if (aead == NULL) {
        return;
    }
    provider_context = (EVP_AEAD_CTX *)aead->provider_context;
    allocator        = aead->allocator;
    if (provider_context != NULL) {
        EVP_AEAD_CTX_cleanup(provider_context);
        utp_allocator_free(allocator, provider_context);
    }
    OPENSSL_cleanse(aead, sizeof(*aead));
}

utp_internal_error_t utp_crypto_aead_init(utp_crypto_aead_t *aead, const utp_allocator_t *allocator, const uint8_t *key,
                                          size_t key_size, const uint8_t nonce_prefix[UTP_CRYPTO_NONCE_PREFIX_SIZE]) {
    const EVP_AEAD        *provider_aead;
    EVP_AEAD_CTX          *provider_context;
    const utp_allocator_t *resolved_allocator;

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

utp_internal_error_t utp_crypto_aead_seal(const utp_crypto_aead_t *aead, uint64_t packet_number,
                                          const uint8_t *plaintext, size_t plaintext_length, const uint8_t *aad,
                                          size_t aad_length, uint8_t *ciphertext, size_t ciphertext_capacity,
                                          size_t *ciphertext_length) {
    uint8_t       nonce[UTP_CRYPTO_AEAD_NONCE_SIZE];
    EVP_AEAD_CTX *provider_context;
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

    provider_context = (EVP_AEAD_CTX *)aead->provider_context;
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

utp_internal_error_t utp_crypto_aead_open(const utp_crypto_aead_t *aead, uint64_t packet_number,
                                          const uint8_t *ciphertext, size_t ciphertext_length, const uint8_t *aad,
                                          size_t aad_length, uint8_t *plaintext, size_t plaintext_capacity,
                                          size_t *plaintext_length) {
    uint8_t       nonce[UTP_CRYPTO_AEAD_NONCE_SIZE];
    EVP_AEAD_CTX *provider_context;
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

    provider_context = (EVP_AEAD_CTX *)aead->provider_context;
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
