/*************************************************************************
    > File Name: md5_mbedtls.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月07日 星期一 11时59分41秒
 ************************************************************************/

#include "md5.h"

#include <string.h>

#if defined(HAVE_MBEDTLS)

#include <mbedtls/md5.h>

#include "crypto_utils.h"

namespace eular {
namespace crypto {
class MD5Context {
public:
    ~MD5Context() {
        mbedtls_md5_free(&ctx);
    }

    mbedtls_md5_context ctx;
};

std::string Hash(const void *data, int32_t bytes)
{
    if (data == nullptr || bytes <= 0) {
        return std::string();
    }

    uint8_t digest[MD5_DIGEST_LENGTH];
    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    if (mbedtls_md5(ptr, bytes, digest)) {
        return std::string();
    }

    return utils::Hex2String(digest, MD5_DIGEST_LENGTH);
}

int32_t MD5::init()
{
    if (m_context == nullptr) {
        m_context = std::unique_ptr<MD5Context>(new MD5Context());
    }

    mbedtls_md5_init(&m_context->ctx);
    return 0;
}

int32_t MD5::update(const void *data, int32_t len)
{
    if (m_context == nullptr || data == nullptr || len <= 0) {
        return -1;
    }

    return mbedtls_md5_update(&m_context->ctx, (const uint8_t *)data, len);
}

int32_t MD5::finalize(void *digest)
{
    if (m_context == nullptr || digest == nullptr) {
        return -1;
    }

    uint8_t md[MD5_DIGEST_LENGTH];
    if (!mbedtls_md5_finish(&m_context->ctx, md)) {
        return -1;
    }

    memcpy(digest, md, MD5_DIGEST_LENGTH);
    return 0;
}

std::string MD5::finalize()
{
    if (m_context == nullptr) {
        return std::string();
    }

    uint8_t md[MD5_DIGEST_LENGTH];
    if (!mbedtls_md5_finish(&m_context->ctx, md)) {
        return std::string();
    }
    return utils::Hex2String(md, MD5_DIGEST_LENGTH);
}

} // namespace crypto
} // namespace eular


#endif