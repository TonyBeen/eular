/*************************************************************************
    > File Name: md5_gnutls.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月07日 星期一 17时38分08秒
 ************************************************************************/

#include "md5.h"

#include <string.h>

#if defined(HAVE_GNUTLS)
#include <nettle/md5.h>
#include "crypto_utils.h"

namespace eular {
namespace crypto {
class MD5Context {
public:
    md5_ctx     ctx;
};

std::string Hash(const void *data, int32_t bytes)
{
    if (data == nullptr || bytes <= 0) {
        return std::string();
    }

    MD5 md5;
    md5.init();
    md5.update(data, bytes);
    return md5.finalize();
}

int32_t MD5::init()
{
    if (m_context == nullptr) {
        m_context = std::unique_ptr<MD5Context>(new MD5Context());
    }

    md5_init(&m_context->ctx);
    return 0;
}

int32_t MD5::update(const void *data, int32_t len)
{
    if (m_context == nullptr || data == nullptr || len <= 0) {
        return -1;
    }

    md5_update(&m_context->ctx, len, (const uint8_t *)data);
    return 0;
}

int32_t MD5::finalize(void *digest)
{
    if (m_context == nullptr || digest == nullptr) {
        return -1;
    }

    uint8_t md[MD5_DIGEST_LENGTH];
    md5_digest(&m_context->ctx, MD5_DIGEST_LENGTH, md);
    memcpy(digest, md, MD5_DIGEST_LENGTH);
    return 0;
}

std::string MD5::finalize()
{
    if (m_context == nullptr) {
        return std::string();
    }

    uint8_t md[MD5_DIGEST_LENGTH];
    md5_digest(&m_context->ctx, MD5_DIGEST_LENGTH, md);
    return utils::Hex2String(md, MD5_DIGEST_LENGTH);
}

} // namespace crypto
} // namespace eular

#endif