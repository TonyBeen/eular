/*************************************************************************
    > File Name: md5_openssl.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月07日 星期一 11时36分35秒
 ************************************************************************/

#include "md5.h"

#include <string.h>

#if defined(HAVE_OPENSSL)
#undef MD5_DIGEST_LENGTH
#include <openssl/md5.h>

#include "crypto_utils.h"

namespace eular {
namespace crypto {
class MD5Context {
public:
    MD5_CTX     ctx;
};

std::string MD5::Hash(const void *data, int32_t bytes)
{
    if (data == nullptr || bytes <= 0) {
        return std::string();
    }

    uint8_t digest[MD5_DIGEST_LENGTH];
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, data, bytes);
    MD5_Final(digest, &ctx);

    return utils::Hex2String(digest, MD5_DIGEST_LENGTH);
}

int32_t MD5::init()
{
    if (m_context == nullptr) {
        m_context = std::unique_ptr<MD5Context>(new MD5Context());
    }

    return MD5_Init(&m_context->ctx) ? 0 : -1;
}

int32_t MD5::update(const void *data, int32_t len)
{
    if (m_context == nullptr || data == nullptr || len <= 0) {
        return -1;
    }

    return MD5_Update(&m_context->ctx, data, len) ? 0 : -1;
}

int32_t MD5::finalize(std::array<uint8_t, MD5_DIGEST_LENGTH> &digest)
{
    if (m_context == nullptr) {
        return -1;
    }

    uint8_t md[MD5_DIGEST_LENGTH];
    if (!MD5_Final(md, &m_context->ctx)) {
        return -1;
    }

    memcpy(&digest[0], md, MD5_DIGEST_LENGTH);
    return 0;
}

int32_t MD5::finalize(std::vector<uint8_t> &digest)
{
    if (m_context == nullptr) {
        return -1;
    }

    uint8_t md[MD5_DIGEST_LENGTH];
    if (!MD5_Final(md, &m_context->ctx)) {
        return -1;
    }

    digest.resize(MD5_DIGEST_LENGTH);
    memcpy(&digest[0], md, MD5_DIGEST_LENGTH);
    return 0;
}

std::string MD5::finalize()
{
    if (m_context == nullptr) {
        return std::string();
    }

    uint8_t md[MD5_DIGEST_LENGTH];
    if (!MD5_Final(md, &m_context->ctx)) {
        return std::string();
    }
    return utils::Hex2String(md, MD5_DIGEST_LENGTH);
}

} // namespace crypto
} // namespace eular

#endif
