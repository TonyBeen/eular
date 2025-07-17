/*************************************************************************
    > File Name: sha_gnutls.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月04日 星期五 16时30分15秒
 ************************************************************************/


#include "sha.h"

#include <string.h>

#if defined(HAVE_GNUTLS)
#include <nettle/sha1.h>
#include <nettle/sha2.h>

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
        sha1_ctx    sha1;
        sha256_ctx  sha256;
        sha512_ctx  sha512;
    } ctx;
};

std::string SHA::Hash(int32_t type, const void *data, int32_t bytes)
{
    if (data == nullptr || bytes <= 0) {
        return std::string();
    }

    SHA sha;
    std::string hash;
    do {
        if (sha.init(type) != 0) {
            break;
        }
        if (sha.update(data, bytes) != 0) {
            break;
        }
        if (sha.finalize(hash) != 0) {
            break;
        }
    } while (0);
    return hash;
}

int32_t SHA::init(int32_t type)
{
    m_context = std::unique_ptr<SHAContext>(new SHAContext());
    int32_t status = 0;
    m_context->type = type;
    switch (type) {
    case SHA_1:
        sha1_init(&m_context->ctx.sha1);
        break;
    case SHA_256:
        sha256_init(&m_context->ctx.sha256);
        break;
    case SHA_512:
        sha512_init(&m_context->ctx.sha512);
        break;
    default:
        status = -1; // Invalid type
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
    switch (m_context->type) {
    case SHA_1:
        sha1_update(&m_context->ctx.sha1, length, ptr);
        break;
    case SHA_256:
        sha256_update(&m_context->ctx.sha256, length, ptr);
        break;
    case SHA_512:
        sha512_update(&m_context->ctx.sha512, length, ptr);
        break;
    default:
        return -1;
    }

    return 0;
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

    switch (m_context->type) {
    case SHA_1:
        sha1_digest(&m_context->ctx.sha1, length, static_cast<uint8_t *>(hash));
        break;
    case SHA_256:
        sha256_digest(&m_context->ctx.sha256, length, static_cast<uint8_t *>(hash));
        break;
    case SHA_512:
        sha512_digest(&m_context->ctx.sha512, length, static_cast<uint8_t *>(hash));
        break;
    default:
        return -1;
    }

    return 0;
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