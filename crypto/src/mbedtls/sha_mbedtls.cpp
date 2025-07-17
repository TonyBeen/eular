/*************************************************************************
    > File Name: sha_mbedtls.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月04日 星期五 16时07分30秒
 ************************************************************************/

#include "sha.h"

#include <string.h>

#if defined(HAVE_MBEDTLS)
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

#include "crypto_utils.h"

namespace eular {
namespace crypto {
class SHAContext {
public:
    SHAContext() {
        memset(&ctx, 0, sizeof(ctx));
    }

    ~SHAContext() {
        switch (type) {
        case SHA::SHA_1:
            mbedtls_sha1_free(&ctx.sha1);
            break;
        case SHA::SHA_256:
            mbedtls_sha256_free(&ctx.sha256);
            break;
        case SHA::SHA_512:
            mbedtls_sha512_free(&ctx.sha512);
            break;
        default:
            break;
        }
    }

    int32_t type = 0;
    union {
        mbedtls_sha1_context    sha1;
        mbedtls_sha256_context  sha256;
        mbedtls_sha512_context  sha512;
    } ctx;
};

std::string SHA::Hash(int32_t type, const void *data, int32_t bytes)
{
    if (data == nullptr || bytes <= 0) {
        return std::string();
    }

    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    int32_t result = 0;

    uint8_t digest[SHA512_DIGEST_LENGTH] = {0};
    int32_t digest_length = 0;
    switch (type) {
    case SHA_1:
        digest_length = SHA_DIGEST_LENGTH;
        result = mbedtls_sha1(ptr, bytes, digest);
        break;
    case SHA_256:
        digest_length = SHA256_DIGEST_LENGTH;
        result = mbedtls_sha256(ptr, bytes, digest, 0);
        break;
    case SHA_512:
        digest_length = SHA512_DIGEST_LENGTH;
        result = mbedtls_sha512(ptr, bytes, digest, 0);
        break;
    default:
        break;
    }

    if (!result) {
        return std::string();
    }

    return utils::Hex2String(digest, digest_length);
}

int32_t SHA::init(int32_t type)
{
    m_context = std::unique_ptr<SHAContext>(new SHAContext());
    int32_t status = 0;
    m_context->type = type;
    switch (type) {
    case SHA_1:
        status = mbedtls_sha1_starts(&m_context->ctx.sha1);
        break;
    case SHA_256:
        status = mbedtls_sha256_starts(&m_context->ctx.sha256, 0);
        break;
    case SHA_512:
        status = mbedtls_sha512_starts(&m_context->ctx.sha512, 0);
        break;
    default:
        break;
    }

    return status;
}

int32_t SHA::update(const void *data, int32_t length)
{
    if (m_context == nullptr || data == nullptr || length <= 0) {
        return -1;
    }

    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    int32_t status = 0;

    switch (m_context->type) {
    case SHA_1:
        status = mbedtls_sha1_update(&m_context->ctx.sha1, ptr, length);
        break;
    case SHA_256:
        status = mbedtls_sha256_update(&m_context->ctx.sha256, ptr, length);
        break;
    case SHA_512:
        status = mbedtls_sha512_update(&m_context->ctx.sha512, ptr, length);
        break;
    default:
        return -1;
    }

    return status;
}

int32_t SHA::update(const std::string &data)
{
    return update(data.data(), static_cast<int32_t>(data.size()));
}

int32_t SHA::finalize(void *hash, int32_t length)
{
    if (m_context == nullptr || hash == nullptr || length <= 0) {
        return -1;
    }

    int32_t status = 0;
    switch (m_context->type) {
    case SHA_1:
        status = mbedtls_sha1_finish(&m_context->ctx.sha1, static_cast<uint8_t *>(hash));
        break;
    case SHA_256:
        status = mbedtls_sha256_finish(&m_context->ctx.sha256, static_cast<uint8_t *>(hash));
        break;
    case SHA_512:
        status = mbedtls_sha512_finish(&m_context->ctx.sha512, static_cast<uint8_t *>(hash));
        break;
    default:
        return -1;
    }

    return status;
}

int32_t SHA::finalize(std::string &hash)
{
    if (m_context == nullptr) {
        return -1;
    }
    uint8_t digest[SHA512_DIGEST_LENGTH] = {0};
    int32_t result = finalize(digest, sizeof(digest));
    if (result != 0) {
        return result;
    }

    hash = utils::Hex2String(digest, hashSize());
    return 0;
}

int32_t SHA::hashSize() const
{
    if (m_context == nullptr) {
        return -1;
    }

    switch (m_context->type) {
    case SHA_1:
        return SHA_DIGEST_LENGTH;
    case SHA_256:
        return SHA256_DIGEST_LENGTH;
    case SHA_512:
        return SHA512_DIGEST_LENGTH;
    default:
        return -1;
    }
}

} // namespace crypto
} // namespace eular

#endif