/*************************************************************************
    > File Name: aes_openssl.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月09日 星期三 17时27分41秒
 ************************************************************************/

#include "aes.h"

#include <assert.h>

#if defined(HAVE_MBEDTLS)

#include <mbedtls/aes.h>

namespace eular {
namespace crypto {
class AESContext {
public:
    AESContext() {
        mbedtls_aes_init(&_ctx);
        _key_size = AES::KeySize::AES_256;
        _encrypt_mode = AES::EncryptMode::ECB;
        memset(_iv, 0, CBC_IV_SIZE);
    }

    ~AESContext() {
        mbedtls_aes_free(&_ctx);
    }

    mbedtls_aes_context _ctx;
    uint8_t             _user_key[AES::KeySize::AES_256];
    uint8_t             _key_size;
    uint8_t             _encrypt_mode;
    uint8_t             _iv[CBC_IV_SIZE];
};

AES::AES() :
    m_context(new AESContext())
{
}

AES::~AES()
{
    m_context.reset();
}

AES::AES(AES &&other)
{
    std::swap(m_context, other.m_context);
}

AES &AES::operator=(AES &&other)
{
    if (this != std::addressof(other)) {
        std::swap(m_context, other.m_context);
    }

    return *this;
}

std::vector<uint8_t> AES::Encrypt(const std::string &data, const std::string &key, int32_t keySize, EncryptMode mode, const void *iv)
{
    AES aes;
    aes.setKey(key, keySize);
    aes.setMode(mode);
    aes.setIV(iv);
    return aes.encrypt(data.data(), data.size());
}

std::vector<uint8_t> AES::Decrypt(const std::string &data, const std::string &key, int32_t keySize, EncryptMode mode, const void *iv)
{
    AES aes;
    aes.setKey(key, keySize);
    aes.setMode(mode);
    aes.setIV(iv);
    return aes.decrypt(data.data(), data.size());
}

void AES::setKey(const std::string &key, int32_t keySize)
{
    if (keySize != KeySize::AES_128 || keySize != KeySize::AES_256) {
        return; // Invalid key size
    }

    m_context->_key_size = static_cast<uint8_t>(keySize);
    memset(m_context->_user_key, 0, AES::KeySize::AES_256);
    size_t copySize = std::min(key.size(), static_cast<size_t>(AES::KeySize::AES_256));
    memcpy(m_context->_user_key, key.data(), copySize);
}

void AES::setIV(const void *iv)
{
    if (iv) {
        memcpy(m_context->_iv, iv, CBC_IV_SIZE);
    } else {
        memset(m_context->_iv, 0, CBC_IV_SIZE); // Default IV to zero if not provided
    }
}

void AES::setMode(EncryptMode mode)
{
    switch (mode) {
    case EncryptMode::ECB:
    case EncryptMode::CBC:
        m_context->_encrypt_mode = static_cast<uint8_t>(mode);
        break;
    default:
        break;
    }
}

std::vector<uint8_t> AES::encrypt(const void *data, size_t len)
{
    std::vector<uint8_t> encryptedData;
    if (m_context == nullptr || data == nullptr || len == 0) {
        return encryptedData;
    }

    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    mbedtls_aes_setkey_enc(&m_context->_ctx, m_context->_user_key, m_context->_key_size * 8);

    int32_t paddingSize = PKCS7_PADDING(len);
    encryptedData.resize(len + paddingSize);
    size_t offset = 0;
    size_t outputOffset = 0;
    if (m_context->_encrypt_mode == EncryptMode::ECB) {
        while (offset < len) {
            size_t blockSize = std::min(len - offset, static_cast<size_t>(AES_BLOCK_SIZE));
            if (blockSize < AES_BLOCK_SIZE) {
                break;
            }

            mbedtls_aes_crypt_ecb(&m_context->_ctx, MBEDTLS_AES_ENCRYPT, ptr + offset, encryptedData.data() + outputOffset);
            offset += AES_BLOCK_SIZE;
            outputOffset += AES_BLOCK_SIZE;
        }

        // Handle padding
        uint8_t temporaryBuffer[AES_BLOCK_SIZE] = {0};
        int64_t remaining = (int64_t)len - (int64_t)offset;
        memcpy(temporaryBuffer, ptr + offset, remaining);
        memset(temporaryBuffer + remaining, (uint8_t)paddingSize, paddingSize);
        mbedtls_aes_crypt_ecb(&m_context->_ctx, MBEDTLS_AES_ENCRYPT, temporaryBuffer, encryptedData.data() + outputOffset);
    } else if (m_context->_encrypt_mode == EncryptMode::CBC) {
        uint8_t temporaryIV[CBC_IV_SIZE];
        memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);

        size_t blockSize = len / AES_BLOCK_SIZE * AES_BLOCK_SIZE;
        mbedtls_aes_crypt_cbc(&m_context->_ctx, MBEDTLS_AES_ENCRYPT, blockSize, temporaryIV, ptr, encryptedData.data());
        offset += blockSize;
        outputOffset += blockSize;

        // Handle padding
        memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);
        uint8_t temporaryBuffer[AES_BLOCK_SIZE] = {0};
        int64_t remaining = (int64_t)len - (int64_t)offset;
        memcpy(temporaryBuffer, ptr + offset, remaining);
        memset(temporaryBuffer + remaining, (uint8_t)paddingSize, paddingSize);

        mbedtls_aes_crypt_cbc(&m_context->_ctx, MBEDTLS_AES_ENCRYPT, AES_BLOCK_SIZE, temporaryIV, temporaryBuffer, temporaryBuffer);
    }

    return encryptedData;
}

std::vector<uint8_t> AES::decrypt(const void *data, size_t len)
{
    std::vector<uint8_t> decryptedData;
    if (m_context == nullptr || data == nullptr || len == 0) {
        return decryptedData;
    }

    assert(len % AES_BLOCK_SIZE == 0); // Ensure length is a multiple of AES block size
    if (len % AES_BLOCK_SIZE != 0) {
        return decryptedData; // Invalid length for AES decryption
    }


    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    mbedtls_aes_setkey_dec(&m_context->_ctx, m_context->_user_key, m_context->_key_size * 8);
    decryptedData.resize(len);
    if (m_context->_encrypt_mode == EncryptMode::ECB) {
        size_t offset = 0;
        size_t outputOffset = 0;
        while (offset < len) {
            size_t blockSize = std::min(len - offset, static_cast<size_t>(AES_BLOCK_SIZE));
            if (blockSize < AES_BLOCK_SIZE) {
                break;
            }

            mbedtls_aes_crypt_ecb(&m_context->_ctx, MBEDTLS_AES_DECRYPT, ptr + offset, decryptedData.data() + outputOffset);
            offset += AES_BLOCK_SIZE;
            outputOffset += AES_BLOCK_SIZE;
        }
    } else if (m_context->_encrypt_mode == EncryptMode::CBC) {
        uint8_t temporaryIV[CBC_IV_SIZE];
        memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);

        size_t blockSize = len;
        mbedtls_aes_crypt_cbc(&m_context->_ctx, MBEDTLS_AES_DECRYPT, blockSize, temporaryIV, ptr, decryptedData.data());
    }

    // Remove padding
    int32_t paddingSize = decryptedData.back();
    if (paddingSize > 0 && paddingSize <= AES_BLOCK_SIZE) {
        decryptedData.resize(decryptedData.size() - paddingSize);
    }
    return decryptedData;
}

}
}

#endif // defined(HAVE_MBEDTLS)
