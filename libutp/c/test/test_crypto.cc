#define CATCH_CONFIG_MAIN

#include <algorithm>
#include <array>
#include <catch2/catch.hpp>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "internal/crypto.h"
}

namespace {

struct allocation_tracker {
    size_t allocations = 0;
    size_t frees       = 0;
};

void *tracked_alloc(void *user_data, size_t size) {
    auto *tracker = static_cast<allocation_tracker *>(user_data);
    ++tracker->allocations;
    return std::malloc(size);
}

void *tracked_realloc(void *user_data, void *pointer, size_t size) {
    auto *tracker = static_cast<allocation_tracker *>(user_data);
    ++tracker->allocations;
    return std::realloc(pointer, size);
}

void tracked_free(void *user_data, void *pointer) {
    auto *tracker = static_cast<allocation_tracker *>(user_data);
    ++tracker->frees;
    std::free(pointer);
}

}  // namespace

TEST_CASE("crypto generates an X25519 key pair", "[crypto]") {
    utp_crypto_key_pair_t key_pair = {};

    REQUIRE(utp_crypto_key_pair_generate(&key_pair) == UTP_INTERNAL_ERROR_OK);

    utp_crypto_key_pair_clear(&key_pair);
}

TEST_CASE("X25519 derives equal non-zero shared secrets", "[crypto]") {
    utp_crypto_key_pair_t                           client        = {};
    utp_crypto_key_pair_t                           server        = {};
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE> client_secret = {};
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE> server_secret = {};

    REQUIRE(utp_crypto_key_pair_generate(&client) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_key_pair_generate(&server) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_x25519_derive(client_secret.data(), client.private_key, server.public_key) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_x25519_derive(server_secret.data(), server.private_key, client.public_key) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(client_secret == server_secret);
    REQUIRE(std::any_of(client_secret.begin(), client_secret.end(), [](uint8_t value) { return value != 0; }));

    utp_crypto_key_pair_clear(&client);
    utp_crypto_key_pair_clear(&server);
}

TEST_CASE("X25519 rejects an all-zero peer public key", "[crypto]") {
    utp_crypto_key_pair_t                           client          = {};
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE> peer_public_key = {};
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE> shared_secret   = {};

    REQUIRE(utp_crypto_key_pair_generate(&client) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_x25519_derive(shared_secret.data(), client.private_key, peer_public_key.data()) ==
            UTP_INTERNAL_ERROR_CRYPTO);
    REQUIRE(std::all_of(shared_secret.begin(), shared_secret.end(), [](uint8_t value) { return value == 0; }));

    utp_crypto_key_pair_clear(&client);
}

TEST_CASE("traffic material binds the transcript and isolates directions", "[crypto]") {
    utp_crypto_key_pair_t                           client        = {};
    utp_crypto_key_pair_t                           server        = {};
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE> shared_secret = {};
    utp_crypto_traffic_material_t                   material      = {};
    utp_crypto_traffic_material_t                   altered       = {};

    REQUIRE(utp_crypto_key_pair_generate(&client) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_key_pair_generate(&server) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_x25519_derive(shared_secret.data(), client.private_key, server.public_key) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_derive_traffic_material(&material, shared_secret.data(), client.public_key, server.public_key,
                                               1001u, 2001u, UTP_CRYPTO_TYPE_AES_GCM_256,
                                               UTP_CRYPTO_AES_256_KEY_SIZE) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_derive_traffic_material(&altered, shared_secret.data(), client.public_key, server.public_key,
                                               1001u, 2002u, UTP_CRYPTO_TYPE_AES_GCM_256,
                                               UTP_CRYPTO_AES_256_KEY_SIZE) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(material.key_size == UTP_CRYPTO_AES_256_KEY_SIZE);
    REQUIRE(std::memcmp(material.client_to_server.key, material.server_to_client.key, material.key_size) != 0);
    REQUIRE(std::memcmp(material.client_to_server.key, altered.client_to_server.key, material.key_size) != 0);

    utp_crypto_traffic_material_clear(&material);
    utp_crypto_traffic_material_clear(&altered);
    utp_crypto_key_pair_clear(&client);
    utp_crypto_key_pair_clear(&server);
}

TEST_CASE("traffic material matches the C++ key schedule vector", "[crypto]") {
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE>        shared_secret                 = {};
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE>        client_public_key             = {};
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE>        server_public_key             = {};
    const std::array<uint8_t, UTP_CRYPTO_AES_256_KEY_SIZE> expected_client_to_server_key = {
        0x38, 0x62, 0x1e, 0xf2, 0x37, 0x09, 0x30, 0xaf, 0x29, 0xf3, 0x15, 0x07, 0x87, 0xcc, 0xca, 0x18,
        0xc5, 0x48, 0x25, 0x2d, 0x14, 0x8d, 0xc6, 0x62, 0x56, 0x77, 0x68, 0x61, 0x95, 0x00, 0x96, 0x2e,
    };
    const std::array<uint8_t, UTP_CRYPTO_NONCE_PREFIX_SIZE> expected_client_to_server_nonce = {
        0x30,
        0x75,
        0x7a,
        0x19,
    };
    const std::array<uint8_t, UTP_CRYPTO_AES_256_KEY_SIZE> expected_server_to_client_key = {
        0x09, 0x20, 0x3d, 0x3b, 0x7a, 0x6e, 0x39, 0xce, 0x6b, 0x2b, 0x4f, 0x67, 0x47, 0x61, 0x76, 0x6c,
        0x17, 0xa6, 0x7a, 0xf6, 0xfe, 0x39, 0x5e, 0xd1, 0x66, 0xd3, 0x72, 0xc4, 0x97, 0x41, 0xfb, 0xe7,
    };
    const std::array<uint8_t, UTP_CRYPTO_NONCE_PREFIX_SIZE> expected_server_to_client_nonce = {
        0xa8,
        0x8d,
        0xc0,
        0xc5,
    };
    utp_crypto_traffic_material_t material = {};

    for (size_t index = 0; index < shared_secret.size(); ++index) {
        shared_secret[index]     = static_cast<uint8_t>(index);
        client_public_key[index] = static_cast<uint8_t>(index + 32u);
        server_public_key[index] = static_cast<uint8_t>(index + 64u);
    }
    REQUIRE(utp_crypto_derive_traffic_material(
                &material, shared_secret.data(), client_public_key.data(), server_public_key.data(), 0x01020304u,
                0xa0b0c0d0u, UTP_CRYPTO_TYPE_AES_GCM_256, UTP_CRYPTO_AES_256_KEY_SIZE) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(std::memcmp(material.client_to_server.key, expected_client_to_server_key.data(),
                        expected_client_to_server_key.size()) == 0);
    REQUIRE(std::memcmp(material.client_to_server.nonce_prefix, expected_client_to_server_nonce.data(),
                        expected_client_to_server_nonce.size()) == 0);
    REQUIRE(std::memcmp(material.server_to_client.key, expected_server_to_client_key.data(),
                        expected_server_to_client_key.size()) == 0);
    REQUIRE(std::memcmp(material.server_to_client.nonce_prefix, expected_server_to_client_nonce.data(),
                        expected_server_to_client_nonce.size()) == 0);

    utp_crypto_traffic_material_clear(&material);
}

TEST_CASE("AES-GCM authenticates ciphertext, AAD, and packet number", "[crypto]") {
    const std::array<uint8_t, UTP_CRYPTO_AES_256_KEY_SIZE> key = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    };
    const std::array<uint8_t, UTP_CRYPTO_NONCE_PREFIX_SIZE>          nonce_prefix        = {0xa0, 0xa1, 0xa2, 0xa3};
    const std::array<uint8_t, 8>                                     plaintext           = {1, 2, 3, 4, 5, 6, 7, 8};
    std::array<uint8_t, 20>                                          aad                 = {};
    std::array<uint8_t, plaintext.size() + UTP_CRYPTO_AEAD_TAG_SIZE> ciphertext          = {};
    std::array<uint8_t, plaintext.size() + UTP_CRYPTO_AEAD_TAG_SIZE> original_ciphertext = {};
    std::array<uint8_t, plaintext.size()>                            decoded             = {};
    utp_crypto_aead_t                                                aead                = {};
    size_t                                                           ciphertext_length   = 0;
    size_t                                                           decoded_length      = 0;

    REQUIRE(utp_crypto_aead_init(&aead, nullptr, key.data(), key.size(), nonce_prefix.data()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_aead_seal(&aead, 7u, plaintext.data(), plaintext.size(), aad.data(), aad.size(),
                                 ciphertext.data(), ciphertext.size(), &ciphertext_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(ciphertext_length == ciphertext.size());
    original_ciphertext = ciphertext;
    REQUIRE(utp_crypto_aead_open(&aead, 7u, ciphertext.data(), ciphertext_length, aad.data(), aad.size(),
                                 decoded.data(), decoded.size(), &decoded_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded_length == plaintext.size());
    REQUIRE(decoded == plaintext);

    ciphertext      = original_ciphertext;
    ciphertext[0]  ^= 0x01u;
    decoded_length  = decoded.size();
    REQUIRE(utp_crypto_aead_open(&aead, 7u, ciphertext.data(), ciphertext_length, aad.data(), aad.size(),
                                 decoded.data(), decoded.size(), &decoded_length) == UTP_INTERNAL_ERROR_CRYPTO);

    ciphertext      = original_ciphertext;
    aad[0]         ^= 0x01u;
    decoded_length  = decoded.size();
    REQUIRE(utp_crypto_aead_open(&aead, 7u, ciphertext.data(), ciphertext_length, aad.data(), aad.size(),
                                 decoded.data(), decoded.size(), &decoded_length) == UTP_INTERNAL_ERROR_CRYPTO);
    aad[0] ^= 0x01u;

    decoded_length = decoded.size();
    REQUIRE(utp_crypto_aead_open(&aead, 8u, ciphertext.data(), ciphertext_length, aad.data(), aad.size(),
                                 decoded.data(), decoded.size(), &decoded_length) == UTP_INTERNAL_ERROR_CRYPTO);

    ciphertext_length = ciphertext.size() - 1u;
    REQUIRE(utp_crypto_aead_seal(&aead, 8u, plaintext.data(), plaintext.size(), aad.data(), aad.size(),
                                 ciphertext.data(), ciphertext_length,
                                 &ciphertext_length) == UTP_INTERNAL_ERROR_OVERFLOW);
    REQUIRE(ciphertext_length == 0u);

    decoded_length = decoded.size() - 1u;
    REQUIRE(utp_crypto_aead_open(&aead, 7u, original_ciphertext.data(), original_ciphertext.size(), aad.data(),
                                 aad.size(), decoded.data(), decoded_length,
                                 &decoded_length) == UTP_INTERNAL_ERROR_OVERFLOW);
    REQUIRE(decoded_length == 0u);

    utp_crypto_aead_cleanup(&aead);
}

TEST_CASE("AES-GCM context uses the supplied allocator", "[crypto]") {
    const std::array<uint8_t, UTP_CRYPTO_AES_128_KEY_SIZE>  key          = {};
    const std::array<uint8_t, UTP_CRYPTO_NONCE_PREFIX_SIZE> nonce_prefix = {};
    allocation_tracker                                      tracker      = {};
    const utp_allocator_t                                   allocator    = {
        tracked_alloc,
        tracked_realloc,
        tracked_free,
        &tracker,
    };
    utp_crypto_aead_t aead = {};

    REQUIRE(utp_crypto_aead_init(&aead, &allocator, key.data(), key.size(), nonce_prefix.data()) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(tracker.allocations == 1u);
    REQUIRE(tracker.frees == 0u);

    utp_crypto_aead_cleanup(&aead);
    REQUIRE(tracker.frees == 1u);
}
