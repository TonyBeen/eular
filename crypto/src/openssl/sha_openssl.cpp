/*************************************************************************
    > File Name: sha_openssl.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月04日 星期五 15时09分00秒
 ************************************************************************/

#include "sha.h"

#include <string.h>

#if defined(HAVE_OPENSSL)
#undef SHA_DIGEST_LENGTH
#undef SHA256_DIGEST_LENGTH
#undef SHA512_DIGEST_LENGTH

#include <openssl/sha.h>

#include "crypto_utils.h"

namespace eular {
namespace crypto {
class SHAContext {
public:
    SHAContext() {
        memset(&ctx, 0, sizeof(ctx));
    }

    int32_t type = 0;
    union {
        SHA_CTX     sha1;
        SHA256_CTX  sha256;
        SHA512_CTX  sha512;
    } ctx;
};

std::string SHA::Hash(int32_t type, const void *data, int32_t bytes)
{
    if (data == nullptr || bytes <= 0) {
        return std::string();
    }

    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    const uint8_t *result = nullptr;

    uint8_t digest[SHA512_DIGEST_LENGTH] = {0};
    int32_t digest_length = 0;
    switch (type) {
    case SHA_1:
        digest_length = SHA_DIGEST_LENGTH;
        result = SHA1(ptr, bytes, digest);
        break;
    case SHA_256:
        digest_length = SHA256_DIGEST_LENGTH;
        result = SHA256(ptr, bytes, digest);
        break;
    case SHA_512:
        digest_length = SHA512_DIGEST_LENGTH;
        result = SHA512(ptr, bytes, digest);
        break;
    default:
        break;
    }

    if (result == nullptr) {
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
        status = SHA1_Init(&m_context->ctx.sha1);
        break;
    case SHA_256:
        status = SHA256_Init(&m_context->ctx.sha256);
        break;
    case SHA_512:
        status = SHA512_Init(&m_context->ctx.sha512);
        break;
    default:
        break;
    }

    return status == 1 ? 0 : -1;
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
        status = SHA1_Update(&m_context->ctx.sha1, ptr, length);
        break;
    case SHA_256:
        status = SHA256_Update(&m_context->ctx.sha256, ptr, length);
        break;
    case SHA_512:
        status = SHA512_Update(&m_context->ctx.sha512, ptr, length);
        break;
    default:
        return -1;
    }

    return status == 1 ? 0 : -1;
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
        status = SHA1_Final(static_cast<uint8_t *>(hash), &m_context->ctx.sha1);
        break;
    case SHA_256:
        status = SHA256_Final(static_cast<uint8_t *>(hash), &m_context->ctx.sha256);
        break;
    case SHA_512:
        status = SHA512_Final(static_cast<uint8_t *>(hash), &m_context->ctx.sha512);
        break;
    default:
        return -1;
    }

    return status == 1 ? 0 : -1;
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