/*************************************************************************
    > File Name: aes.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月07日 星期一 17时51分44秒
 ************************************************************************/

#ifndef __CRYPTO_AES_H__
#define __CRYPTO_AES_H__

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <memory>

#ifndef AES_BLOCK_SIZE
#define AES_BLOCK_SIZE  16
#endif
#define PKCS7_PADDING(x) ((x) % AES_BLOCK_SIZE) == 0 ? AES_BLOCK_SIZE : (AES_BLOCK_SIZE - ((x) % AES_BLOCK_SIZE))
#define CBC_IV_SIZE     16

namespace eular {
namespace crypto {
class AESContext;
class AES
{
public:
    using Ptr = std::unique_ptr<AES>;
    using SP  = std::shared_ptr<AES>;
    using WP  = std::weak_ptr<AES>;

    enum KeySize {
        AES_128 = 16,
        AES_256 = 32
    };
    enum EncryptMode {
        ECB = 0,
        CBC = 1,
    };

    /**
     * @brief Construct a new AES object
     * @default ECB 256.
     */
    AES();
    ~AES();
    AES(const AES&) = delete;
    AES& operator=(const AES&) = delete;
    AES(AES&&);
    AES& operator=(AES&&);

    static std::vector<uint8_t> Encrypt(const std::string &data, const std::string &key, int32_t keySize = AES_256, EncryptMode mode = ECB, const void *iv = nullptr);
    static std::vector<uint8_t> Decrypt(const std::string &data, const std::string &key, int32_t keySize = AES_256, EncryptMode mode = ECB, const void *iv = nullptr);

    /**
     * @brief set key for AES encryption.
     *
     * @param key key
     * @param keySize encryption key size, must be 'AES_128' or 'AES_256'.
     */
    void setKey(const std::string &key, int32_t keySize = AES_256);

    /**
     * @brief CBC mode requires an initialization vector (IV) to be set.
     *
     * @param iv iv pointer, must be 'CBC_IV_SIZE' bytes long.
     */
    void setIV(const void *iv);

    /**
     * @brief Set the encryption mode.
     *
     * @param mode mode, must be 'ECB' or 'CBC'.
     */
    void setMode(EncryptMode mode);

    /**
     * @brief encrypt data using AES.
     *
     * @param data data
     * @param len length
     * @return std::vector<uint8_t> encrypted data
     */
    std::vector<uint8_t> encrypt(const void *data, size_t len);
    template <typename T>
    std::vector<uint8_t> encrypt(const std::vector<T> &data) {
        return encrypt(data.data(), data.size() * sizeof(T));
    }
    template <typename T>
    std::vector<uint8_t> encrypt(const std::basic_string<T> &data) {
        return encrypt(data.data(), data.size() * sizeof(T));
    }

    /**
     * @brief decrypt data using AES.
     *
     * @param data data
     * @param len length
     * @return std::vector<uint8_t> decrypted data
     */
    std::vector<uint8_t> decrypt(const void *data, size_t len);
    template <typename T>
    std::vector<uint8_t> decrypt(const std::vector<T> &data) {
        return decrypt(data.data(), data.size() * sizeof(T));
    }
    template <typename T>
    std::vector<uint8_t> decrypt(const std::basic_string<T> &data) {
        return decrypt(data.data(), data.size() * sizeof(T));
    }

private:
    std::unique_ptr<AESContext>     m_context;
};
} // namespace crypto
} // namespace eular

#endif // __CRYPTO_AES_H__
