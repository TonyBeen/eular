#include "sha256.h"

#include <openssl/evp.h>
#include <openssl/opensslv.h>

#include <stdint.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

static std::string HexDigest(const uint8_t* digest, size_t len)
{
    static const char kHex[] = "0123456789abcdef";
    std::string       out;

    out.reserve(len * 2u);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[digest[i] & 0x0Fu]);
    }
    return out;
}

static std::string LocalSha256Hex(const void* data, size_t len)
{
    uint8_t    digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(digest, &ctx);
    return HexDigest(digest, sizeof(digest));
}

static bool OpenSslSha256(const void* data, size_t len, uint8_t digest[EVP_MAX_MD_SIZE], unsigned int* digest_len)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        return false;
    }

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
              EVP_DigestUpdate(ctx, data, len) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}

static std::string OpenSslSha256Hex(const void* data, size_t len)
{
    uint8_t      digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    if (!OpenSslSha256(data, len, digest, &digest_len)) {
        return "";
    }
    return HexDigest(digest, digest_len);
}

template <typename Fn>
static double MeasureThroughputMiBPerSec(Fn fn, size_t bytes_per_round, size_t rounds)
{
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    fn();
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration_cast<std::chrono::duration<double> >(end - start).count();
    const double mib = static_cast<double>(bytes_per_round * rounds) / (1024.0 * 1024.0);
    return seconds > 0.0 ? mib / seconds : 0.0;
}

}  // namespace

int main()
{
    const size_t block_size = 1024u * 1024u;
    const size_t rounds = 256u;
    std::vector<uint8_t> block(block_size);

    for (size_t i = 0; i < block.size(); ++i) {
        block[i] = static_cast<uint8_t>(i & 0xFFu);
    }

    const std::string local_empty = LocalSha256Hex("", 0);
    const std::string openssl_empty = OpenSslSha256Hex("", 0);
    const std::string local_block = LocalSha256Hex(block.data(), block.size());
    const std::string openssl_block = OpenSslSha256Hex(block.data(), block.size());

    if (local_empty != openssl_empty || local_block != openssl_block) {
        std::fprintf(stderr, "SHA256 mismatch\n");
        std::fprintf(stderr, "local empty:   %s\n", local_empty.c_str());
        std::fprintf(stderr, "openssl empty: %s\n", openssl_empty.c_str());
        std::fprintf(stderr, "local block:   %s\n", local_block.c_str());
        std::fprintf(stderr, "openssl block: %s\n", openssl_block.c_str());
        return 1;
    }

    uint8_t local_digest[SHA256_DIGEST_LENGTH];
    uint8_t openssl_digest[EVP_MAX_MD_SIZE];
    unsigned int openssl_digest_len = 0;

    const double local_mib_per_sec = MeasureThroughputMiBPerSec(
        [&]() {
            SHA256_CTX ctx;
            SHA256_Init(&ctx);
            for (size_t i = 0; i < rounds; ++i) {
                SHA256_Update(&ctx, block.data(), block.size());
            }
            SHA256_Final(local_digest, &ctx);
        },
        block_size, rounds);

    const double openssl_mib_per_sec = MeasureThroughputMiBPerSec(
        [&]() {
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            if (ctx == NULL) {
                return;
            }
            EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
            for (size_t i = 0; i < rounds; ++i) {
                EVP_DigestUpdate(ctx, block.data(), block.size());
            }
            EVP_DigestFinal_ex(ctx, openssl_digest, &openssl_digest_len);
            EVP_MD_CTX_free(ctx);
        },
        block_size, rounds);

    const std::string local_stream = HexDigest(local_digest, sizeof(local_digest));
    const std::string openssl_stream = HexDigest(openssl_digest, openssl_digest_len);
    if (local_stream != openssl_stream) {
        std::fprintf(stderr, "stream SHA256 mismatch\n");
        std::fprintf(stderr, "local stream:   %s\n", local_stream.c_str());
        std::fprintf(stderr, "openssl stream: %s\n", openssl_stream.c_str());
        return 1;
    }

    std::printf("OpenSSL: %s\n", OPENSSL_VERSION_TEXT);
    std::printf("input: %zu MiB total, %zu MiB blocks x %zu rounds\n",
                (block_size * rounds) / (1024u * 1024u), block_size / (1024u * 1024u), rounds);
    std::printf("local sha256:   %.2f MiB/s\n", local_mib_per_sec);
    std::printf("openssl sha256: %.2f MiB/s\n", openssl_mib_per_sec);
    if (local_mib_per_sec > 0.0) {
        std::printf("openssl/local:  %.2fx\n", openssl_mib_per_sec / local_mib_per_sec);
    }

    return 0;
}
