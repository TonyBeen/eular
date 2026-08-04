#define CATCH_CONFIG_MAIN

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include <catch2/catch.hpp>

extern "C" {
#include "crypto/crypto.h"
}

namespace {

struct allocation_tracker {
    size_t allocations = 0;
    size_t frees       = 0;
};

void* tracked_alloc(void* user_data, size_t size)
{
    auto* tracker = static_cast<allocation_tracker*>(user_data);
    ++tracker->allocations;
    return std::malloc(size);
}

void* tracked_realloc(void* user_data, void* pointer, size_t size)
{
    auto* tracker = static_cast<allocation_tracker*>(user_data);
    ++tracker->allocations;
    return std::realloc(pointer, size);
}

void tracked_free(void* user_data, void* pointer)
{
    auto* tracker = static_cast<allocation_tracker*>(user_data);
    ++tracker->frees;
    std::free(pointer);
}

}  // namespace

TEST_CASE("crypto generates an X25519 key pair", "[crypto]")
{
    utp_crypto_key_pair_t key_pair = {};

    REQUIRE(utp_crypto_key_pair_generate(&key_pair) == UTP_INTERNAL_ERROR_OK);

    utp_crypto_key_pair_clear(&key_pair);
}

TEST_CASE("X25519 derives equal non-zero shared secrets", "[crypto]")
{
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

TEST_CASE("X25519 rejects an all-zero peer public key", "[crypto]")
{
    utp_crypto_key_pair_t                           client          = {};
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE> peer_public_key = {};
    std::array<uint8_t, UTP_CRYPTO_X25519_KEY_SIZE> shared_secret   = {};

    REQUIRE(utp_crypto_key_pair_generate(&client) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_crypto_x25519_derive(shared_secret.data(), client.private_key, peer_public_key.data()) ==
            UTP_INTERNAL_ERROR_CRYPTO);
    REQUIRE(std::all_of(shared_secret.begin(), shared_secret.end(), [](uint8_t value) { return value == 0; }));

    utp_crypto_key_pair_clear(&client);
}

TEST_CASE("traffic material binds the transcript and isolates directions", "[crypto]")
{
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

TEST_CASE("traffic material matches the C++ key schedule vector", "[crypto]")
{
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

TEST_CASE("AES-GCM authenticates ciphertext, AAD, and packet number", "[crypto]")
{
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

TEST_CASE("AES-GCM context uses the supplied allocator", "[crypto]")
{
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

TEST_CASE("directional AEAD contexts interoperate in both directions in place", "[crypto]")
{
    const std::array<uint8_t, 2> crypto_types = {
        UTP_CRYPTO_TYPE_AES_GCM_128,
        UTP_CRYPTO_TYPE_AES_GCM_256,
    };
    const std::array<uint8_t, 9>  client_message = {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u};
    const std::array<uint8_t, 7>  server_message = {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u};
    const std::array<uint8_t, 20> aad = {0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 0x36u, 0x37u, 0x38u, 0x39u,
                                         0x3au, 0x3bu, 0x3cu, 0x3du, 0x3eu, 0x3fu, 0x40u, 0x41u, 0x42u, 0x43u};

    for (uint8_t crypto_type : crypto_types) {
        utp_crypto_key_pair_t                                                 client_key_pair  = {};
        utp_crypto_key_pair_t                                                 server_key_pair  = {};
        utp_crypto_aead_t                                                     client_tx        = {};
        utp_crypto_aead_t                                                     client_rx        = {};
        utp_crypto_aead_t                                                     server_tx        = {};
        utp_crypto_aead_t                                                     server_rx        = {};
        std::array<uint8_t, client_message.size() + UTP_CRYPTO_AEAD_TAG_SIZE> client_buffer    = {};
        std::array<uint8_t, server_message.size() + UTP_CRYPTO_AEAD_TAG_SIZE> server_buffer    = {};
        size_t                                                                encrypted_length = 0u;
        size_t                                                                decrypted_length = 0u;

        REQUIRE(utp_crypto_key_pair_generate(&client_key_pair) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_crypto_key_pair_generate(&server_key_pair) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_crypto_create_directional_aead(&client_key_pair, server_key_pair.public_key, 1001u, 2001u,
                                                   crypto_type, true, &client_tx, &client_rx) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_crypto_create_directional_aead(&server_key_pair, client_key_pair.public_key, 1001u, 2001u,
                                                   crypto_type, false, &server_tx,
                                                   &server_rx) == UTP_INTERNAL_ERROR_OK);

        std::copy(client_message.begin(), client_message.end(), client_buffer.begin());
        REQUIRE(utp_crypto_aead_seal(&client_tx, 17u, client_buffer.data(), client_message.size(), aad.data(),
                                     aad.size(), client_buffer.data(), client_buffer.size(),
                                     &encrypted_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(encrypted_length == client_buffer.size());
        REQUIRE(utp_crypto_aead_open(&server_rx, 17u, client_buffer.data(), encrypted_length, aad.data(), aad.size(),
                                     client_buffer.data(), client_buffer.size(),
                                     &decrypted_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(decrypted_length == client_message.size());
        REQUIRE(std::equal(client_message.begin(), client_message.end(), client_buffer.begin()));

        std::copy(server_message.begin(), server_message.end(), server_buffer.begin());
        REQUIRE(utp_crypto_aead_seal(&server_tx, 18u, server_buffer.data(), server_message.size(), aad.data(),
                                     aad.size(), server_buffer.data(), server_buffer.size(),
                                     &encrypted_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(encrypted_length == server_buffer.size());
        REQUIRE(utp_crypto_aead_open(&client_rx, 18u, server_buffer.data(), encrypted_length, aad.data(), aad.size(),
                                     server_buffer.data(), server_buffer.size(),
                                     &decrypted_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(decrypted_length == server_message.size());
        REQUIRE(std::equal(server_message.begin(), server_message.end(), server_buffer.begin()));

        utp_crypto_aead_cleanup(&client_tx);
        utp_crypto_aead_cleanup(&client_rx);
        utp_crypto_aead_cleanup(&server_tx);
        utp_crypto_aead_cleanup(&server_rx);
        utp_crypto_key_pair_clear(&client_key_pair);
        utp_crypto_key_pair_clear(&server_key_pair);
    }
}
