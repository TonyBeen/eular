/*************************************************************************
    > File Name: base64.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月11日 星期五 15时00分15秒
 ************************************************************************/

#include "base64.h"

#include "libbase64.h"

namespace eular {
namespace crypto {
class Base64Context {
public:
    struct base64_state  _ctx;
};

Base64::Base64(Base64 &&other)
{
    std::swap(m_context, other.m_context);
}

Base64& Base64::operator=(Base64 &&other)
{
    if (this != std::addressof(other)) {
        std::swap(m_context, other.m_context);
    }

    return *this;
}

std::string Base64::Encrypt(const void *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return std::string();
    }
    size_t size = base64_encode_len(len);
    std::string result;
    result.resize(size);
    base64_encode((const char *)data, len, const_cast<char *>(result.data()), &size, 0);
    result.resize(size);
    return result;
}

std::vector<uint8_t> Base64::Decrypt(const void *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return std::vector<uint8_t>();
    }

    size_t size = base64_decode_len(len);
    std::vector<uint8_t> result;
    result.resize(size);
    base64_decode((const char *)data, len, reinterpret_cast<char *>(result.data()), &size, 0);
    result.resize(size);
    return result;
}

std::string Base64::encrypt(const void *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return std::string();
    }
    if (m_context == nullptr) {
        m_context = std::unique_ptr<Base64Context>(new Base64Context());
        base64_stream_encode_init(&m_context->_ctx, 0);
    }
    std::string result;
    auto size = base64_encode_len(len);
    result.resize(size);
    base64_stream_encode(&m_context->_ctx, (const char *)data, len,
                         const_cast<char *>(result.data()), &size);
    result.resize(size);
    return result;
}

std::string Base64::encryptFinal()
{
    if (m_context == nullptr) {
        return std::string();
    }

    std::string result;
    char buffer[8] = {0};
    size_t size = sizeof(buffer);
    base64_stream_encode_final(&m_context->_ctx, buffer, &size);
    result.assign(buffer, size);
    return result;
}

} // namespace crypto
} // namespace eular
