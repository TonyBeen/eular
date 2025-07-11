/*************************************************************************
    > File Name: base.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月11日 星期五 11时58分55秒
 ************************************************************************/

#ifndef __CRYPTO_BASE64_H__
#define __CRYPTO_BASE64_H__

#include <stdint.h>
#include <string>
#include <vector>
#include <memory>

#define base64_encode_len(A) (((A) + 2) / 3 * 4)
#define base64_decode_len(A) ( (A) / 4 * 3 + 2)

namespace eular {
namespace crypto {
class Base64Context;
class Base64
{
    Base64(const Base64&) = delete;
    Base64& operator=(const Base64&) = delete;
public:
    Base64();
    ~Base64();

    Base64(Base64 &&other);
    Base64& operator=(Base64 &&other);

    static std::string Encrypt(const void *data, size_t len);
    template<typename T>
    static std::string Encrypt(const std::basic_string<T> &data) {
        return Encrypt(data.data(), data.size() * sizeof(T));
    }
    template<typename T>
    static std::string Encrypt(const std::vector<T> &data) {
        return Encrypt(data.data(), data.size() * sizeof(T));
    }

    static std::vector<uint8_t> Decrypt(const void *data, size_t len);
    static std::vector<uint8_t> Decrypt(const std::string &data) {
        return Decrypt(data.data(), data.size());
    }

    std::string encrypt(const void *data, size_t len);
    std::string encryptFinal();
private:
    std::unique_ptr<Base64Context>  m_context;
};
} // namespace crypto
} // namespace eular

#endif // __CRYPTO_BASE64_H__
