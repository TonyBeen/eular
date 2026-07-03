#include "sha256.h"

#include <stdint.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "catch/catch.hpp"

namespace {

static std::string HexDigest(const uint8_t digest[SHA256_DIGEST_LENGTH])
{
    static const char kHex[] = "0123456789abcdef";
    std::string       out;

    out.reserve(SHA256_DIGEST_LENGTH * 2u);
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[digest[i] & 0x0Fu]);
    }
    return out;
}

static std::string Sha256Hex(const void* data, size_t len)
{
    uint8_t    digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(digest, &ctx);
    return HexDigest(digest);
}

static std::string Sha256Hex(const std::string& data)
{
    return Sha256Hex(data.data(), data.size());
}

static std::string Sha256HexChunked(const std::string& data, size_t chunk_size)
{
    uint8_t    digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;

    SHA256_Init(&ctx);
    for (size_t offset = 0; offset < data.size(); offset += chunk_size) {
        size_t len = data.size() - offset;
        if (len > chunk_size) {
            len = chunk_size;
        }
        SHA256_Update(&ctx, data.data() + offset, len);
    }
    SHA256_Final(digest, &ctx);
    return HexDigest(digest);
}

}  // namespace

TEST_CASE("SHA256 matches standard digest vectors")
{
    REQUIRE(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb924"
                              "27ae41e4649b934ca495991b7852b855");
    REQUIRE(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223"
                                 "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
            "248d6a61d20638b8e5c026930c3e6039"
            "a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA256 produces the same digest for chunked updates")
{
    std::string data;

    data.reserve(4097);
    for (size_t i = 0; i < 4097; ++i) {
        data.push_back(static_cast<char>('A' + (i % 26u)));
    }

    const std::string expected = Sha256Hex(data);
    REQUIRE(Sha256HexChunked(data, 1) == expected);
    REQUIRE(Sha256HexChunked(data, 7) == expected);
    REQUIRE(Sha256HexChunked(data, 64) == expected);
    REQUIRE(Sha256HexChunked(data, 255) == expected);
}

TEST_CASE("SHA256 handles large input correctly")
{
    std::string data(1000000, 'a');

    REQUIRE(Sha256Hex(data) == "cdc76e5c9914fb9281a1c7e284d73e67"
                               "f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA256 throughput smoke test")
{
    const size_t block_size = 1024u * 1024u;
    const size_t rounds = 64u;
    std::vector<uint8_t> block(block_size);
    uint8_t             digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX          ctx;

    for (size_t i = 0; i < block.size(); ++i) {
        block[i] = static_cast<uint8_t>(i & 0xFFu);
    }

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    SHA256_Init(&ctx);
    for (size_t i = 0; i < rounds; ++i) {
        SHA256_Update(&ctx, block.data(), block.size());
    }
    SHA256_Final(digest, &ctx);
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration_cast<std::chrono::duration<double> >(end - start).count();
    const double mib = static_cast<double>(block_size * rounds) / (1024.0 * 1024.0);
    const double mib_per_sec = seconds > 0.0 ? mib / seconds : 0.0;

    REQUIRE(seconds >= 0.0);
    REQUIRE(HexDigest(digest) == "281e519df3077b557c6b03f5da83c4e8"
                                 "d397219259615dd7c3308f89cae8f2a6");
    std::printf("SHA256 throughput: %.2f MiB/s over %.0f MiB\n", mib_per_sec, mib);
}
