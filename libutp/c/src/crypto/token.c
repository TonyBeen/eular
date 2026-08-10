#include "crypto/token.h"

#include <string.h>

#include <openssl/aead.h>
#include <openssl/mem.h>
#include <openssl/rand.h>

#include "proto/wire.h"

static const uint8_t k_zero_rtt_aad[] = "UTP-0RTTToken-AAD";

static void          utp_token_store_u32(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static uint32_t utp_token_load_u32(const uint8_t* input)
{
    return ((uint32_t)input[0] << 24u) | ((uint32_t)input[1] << 16u) | ((uint32_t)input[2] << 8u) | input[3];
}

static void utp_token_encode_meta(const utp_token_meta_t* meta, uint8_t output[UTP_TOKEN_META_SIZE])
{
    output[0] = meta->token_type;
    utp_token_store_u32(output + 1u, meta->timestamp_seconds);
    utp_token_store_u32(output + 5u, meta->cid);
    output[9] = meta->encryption_mode;
    utp_token_store_u32(output + 10u, meta->version);
    utp_token_store_u32(output + 14u, meta->secret);
    output[18] = (uint8_t)(meta->family >> 8u);
    output[19] = (uint8_t)meta->family;
    memcpy(output + 20u, meta->address, sizeof(meta->address));
}

static void utp_token_decode_meta(utp_token_meta_t* meta, const uint8_t input[UTP_TOKEN_META_SIZE])
{
    meta->token_type        = input[0];
    meta->timestamp_seconds = utp_token_load_u32(input + 1u);
    meta->cid               = utp_token_load_u32(input + 5u);
    meta->encryption_mode   = input[9];
    meta->version           = utp_token_load_u32(input + 10u);
    meta->secret            = utp_token_load_u32(input + 14u);
    meta->family            = (uint16_t)(((uint16_t)input[18] << 8u) | input[19]);
    memcpy(meta->address, input + 20u, sizeof(meta->address));
}

static utp_internal_error_t utp_token_auth_rotate(utp_token_auth_t* auth, uint64_t now_seconds)
{
    if (!auth->initialized || now_seconds < auth->key_updated_seconds ||
        now_seconds - auth->key_updated_seconds < UTP_TOKEN_KEY_ROTATION_SECONDS) {
        return UTP_INTERNAL_ERROR_OK;
    }
    memcpy(auth->old_key, auth->key, sizeof(auth->key));
    if (RAND_bytes(auth->key, (int)sizeof(auth->key)) != 1) {
        memcpy(auth->key, auth->old_key, sizeof(auth->key));
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    auth->key_updated_seconds = now_seconds;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_token_auth_init(utp_token_auth_t* auth, uint64_t now_seconds)
{
    if (auth == NULL || now_seconds == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    OPENSSL_cleanse(auth, sizeof(*auth));
    if (RAND_bytes(auth->key, (int)sizeof(auth->key)) != 1) {
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    memcpy(auth->old_key, auth->key, sizeof(auth->key));
    auth->key_updated_seconds = now_seconds;
    auth->initialized         = true;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_token_auth_cleanup(utp_token_auth_t* auth)
{
    if (auth != NULL) {
        OPENSSL_cleanse(auth, sizeof(*auth));
    }
}

static utp_internal_error_t utp_token_auth_crypt(const uint8_t key[UTP_TOKEN_KEY_SIZE], const uint8_t nonce[12],
                                                 const uint8_t* input, size_t input_length, uint8_t* output,
                                                 size_t output_capacity, bool seal)
{
    EVP_AEAD_CTX context;
    size_t       output_length = 0u;
    int          result;

    EVP_AEAD_CTX_zero(&context);
    result = EVP_AEAD_CTX_init(&context, EVP_aead_aes_256_gcm(), key, UTP_TOKEN_KEY_SIZE, UTP_TOKEN_TAG_SIZE, NULL);
    if (result == 1) {
        if (seal) {
            result = EVP_AEAD_CTX_seal(&context, output, &output_length, output_capacity, nonce, UTP_TOKEN_NONCE_SIZE,
                                       input, input_length, k_zero_rtt_aad, sizeof(k_zero_rtt_aad) - 1u);
        } else {
            result = EVP_AEAD_CTX_open(&context, output, &output_length, output_capacity, nonce, UTP_TOKEN_NONCE_SIZE,
                                       input, input_length, k_zero_rtt_aad, sizeof(k_zero_rtt_aad) - 1u);
        }
    }
    EVP_AEAD_CTX_cleanup(&context);
    if (result != 1 || output_length != (seal ? UTP_TOKEN_META_SIZE + UTP_TOKEN_TAG_SIZE : UTP_TOKEN_META_SIZE)) {
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_token_auth_seal(utp_token_auth_t* auth, const utp_token_meta_t* meta,
                                         uint8_t token[UTP_TOKEN_SIZE], uint64_t now_seconds)
{
    uint8_t              plaintext[UTP_TOKEN_META_SIZE];
    utp_internal_error_t error;

    if (auth == NULL || meta == NULL || token == NULL || !auth->initialized ||
        meta->token_type != UTP_TOKEN_TYPE_ZERO_RTT) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_token_auth_rotate(auth, now_seconds);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (RAND_bytes(token, UTP_TOKEN_NONCE_SIZE) != 1) {
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    utp_token_encode_meta(meta, plaintext);
    error = utp_token_auth_crypt(auth->key, token, plaintext, sizeof(plaintext), token + UTP_TOKEN_NONCE_SIZE,
                                 UTP_TOKEN_SIZE - UTP_TOKEN_NONCE_SIZE, true);
    OPENSSL_cleanse(plaintext, sizeof(plaintext));
    return error;
}

utp_internal_error_t utp_token_auth_open(utp_token_auth_t* auth, const uint8_t token[UTP_TOKEN_SIZE],
                                         uint8_t expected_type, utp_token_meta_t* out_meta, uint64_t now_seconds)
{
    uint8_t              plaintext[UTP_TOKEN_META_SIZE];
    utp_internal_error_t error;

    if (auth == NULL || token == NULL || out_meta == NULL || !auth->initialized ||
        expected_type != UTP_TOKEN_TYPE_ZERO_RTT) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_token_auth_rotate(auth, now_seconds);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_token_auth_crypt(auth->key, token, token + UTP_TOKEN_NONCE_SIZE,
                                 UTP_TOKEN_META_SIZE + UTP_TOKEN_TAG_SIZE, plaintext, sizeof(plaintext), false);
    if (error != UTP_INTERNAL_ERROR_OK) {
        error = utp_token_auth_crypt(auth->old_key, token, token + UTP_TOKEN_NONCE_SIZE,
                                     UTP_TOKEN_META_SIZE + UTP_TOKEN_TAG_SIZE, plaintext, sizeof(plaintext), false);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        utp_token_decode_meta(out_meta, plaintext);
        if (out_meta->token_type != expected_type) {
            error = UTP_INTERNAL_ERROR_AUTH;
        }
    }
    OPENSSL_cleanse(plaintext, sizeof(plaintext));
    return error;
}

bool utp_token_meta_address_matches(const utp_token_meta_t* meta, const utp_address_t* address)
{
    if (meta == NULL || address == NULL || meta->family != address->family) {
        return false;
    }
    if (address->family == UTP_ADDRESS_FAMILY_IPV4) {
        const size_t length = 4u;
        return memcmp(meta->address, address->address, length) == 0;
    } else if (address->family == UTP_ADDRESS_FAMILY_IPV6) {
        const size_t length = 16u;
        return memcmp(meta->address, address->address, length) == 0;
    } else {
        return false;
    }
}
