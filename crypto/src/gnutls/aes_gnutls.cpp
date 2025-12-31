/*************************************************************************
    > File Name: aes_gnutls.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月10日 星期四 14时39分24秒
 ************************************************************************/

#include "aes.h"

#include <assert.h>

#if defined(HAVE_GNUTLS)

#include <nettle/aes.h>
#include <nettle/cbc.h>

namespace eular {
namespace crypto {
class AESContext {
public:
    AESContext() {
        _key_size = AES::KeySize::AES_256;
        _encrypt_mode = AES::EncryptMode::ECB;
        memset(_iv, 0, CBC_IV_SIZE);
    }

    ~AESContext() {
    }

    union {
        aes128_ctx  _aes128;
        aes256_ctx  _aes256;
    } _ctx;
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
    int32_t paddingSize = PKCS7_PADDING(len);
    encryptedData.resize(len + paddingSize);
    size_t offset = 0;
    size_t outputOffset = 0;
    if (m_context->_key_size == KeySize::AES_128) {
        aes128_set_encrypt_key(&m_context->_ctx._aes128, m_context->_user_key);
        if (m_context->_encrypt_mode == EncryptMode::ECB) {
            while (offset < len) {
                nettle_aes128_encrypt(&m_context->_ctx._aes128, AES_BLOCK_SIZE, &encryptedData[outputOffset], ptr + offset);
                offset += AES_BLOCK_SIZE;
                outputOffset += AES_BLOCK_SIZE;
            }

            // Handle padding
            uint8_t temporaryBuffer[AES_BLOCK_SIZE] = {0};
            int64_t remaining = (int64_t)len - (int64_t)offset;
            memcpy(temporaryBuffer, ptr + offset, remaining);
            memset(temporaryBuffer + remaining, (uint8_t)paddingSize, paddingSize);
            nettle_aes128_encrypt(&m_context->_ctx._aes128, AES_BLOCK_SIZE, &encryptedData[outputOffset], temporaryBuffer);
        } else if (m_context->_encrypt_mode == EncryptMode::CBC) {
            uint8_t temporaryIV[CBC_IV_SIZE];
            memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);

            size_t blockSize = len / AES_BLOCK_SIZE * AES_BLOCK_SIZE;
            nettle_cbc_encrypt(&m_context->_ctx._aes128, (nettle_cipher_func*)nettle_aes128_encrypt, 
                CBC_IV_SIZE, temporaryIV, blockSize, encryptedData.data(), ptr);
            offset += blockSize;
            outputOffset += blockSize;

            // Handle padding
            memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);
            uint8_t temporaryBuffer[AES_BLOCK_SIZE] = {0};
            int64_t remaining = (int64_t)len - (int64_t)offset;
            blockSize = AES_BLOCK_SIZE;
            memcpy(temporaryBuffer, ptr + offset, remaining);
            memset(temporaryBuffer + remaining, (uint8_t)paddingSize, paddingSize);

            nettle_cbc_encrypt(&m_context->_ctx._aes128, (nettle_cipher_func*)nettle_aes128_encrypt, 
                CBC_IV_SIZE, temporaryIV, blockSize, encryptedData.data() + outputOffset, ptr);
        }
    } else if (m_context->_key_size == KeySize::AES_256) {
        aes256_set_encrypt_key(&m_context->_ctx._aes256, m_context->_user_key);
        if (m_context->_encrypt_mode == EncryptMode::ECB) {
            while (offset < len) {
                nettle_aes256_encrypt(&m_context->_ctx._aes256, AES_BLOCK_SIZE, &encryptedData[outputOffset], ptr + offset);
                offset += AES_BLOCK_SIZE;
                outputOffset += AES_BLOCK_SIZE;
            }

            // Handle padding
            uint8_t temporaryBuffer[AES_BLOCK_SIZE] = {0};
            int64_t remaining = (int64_t)len - (int64_t)offset;
            memcpy(temporaryBuffer, ptr + offset, remaining);
            memset(temporaryBuffer + remaining, (uint8_t)paddingSize, paddingSize);
            nettle_aes256_encrypt(&m_context->_ctx._aes256, AES_BLOCK_SIZE, &encryptedData[outputOffset], temporaryBuffer);
        } else if (m_context->_encrypt_mode == EncryptMode::CBC) {
            uint8_t temporaryIV[CBC_IV_SIZE];
            memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);

            size_t blockSize = len / AES_BLOCK_SIZE * AES_BLOCK_SIZE;
            nettle_cbc_encrypt(&m_context->_ctx._aes256, (nettle_cipher_func*)nettle_aes256_encrypt, 
                CBC_IV_SIZE, temporaryIV, blockSize, encryptedData.data(), ptr);
            offset += blockSize;
            outputOffset += blockSize;

            // Handle padding
            memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);
            uint8_t temporaryBuffer[AES_BLOCK_SIZE] = {0};
            int64_t remaining = (int64_t)len - (int64_t)offset;
            blockSize = AES_BLOCK_SIZE;
            memcpy(temporaryBuffer, ptr + offset, remaining);
            memset(temporaryBuffer + remaining, (uint8_t)paddingSize, paddingSize);

            nettle_cbc_encrypt(&m_context->_ctx._aes256, (nettle_cipher_func*)nettle_aes256_encrypt, 
                CBC_IV_SIZE, temporaryIV, blockSize, encryptedData.data() + outputOffset, ptr);
        }
    }

    return encryptedData;
}

std::vector<uint8_t> AES::decrypt(const void *data, size_t len)
{
    std::vector<uint8_t> decryptedData;
    if (m_context == nullptr || data == nullptr || len == 0) {
        return decryptedData;
    }

    const uint8_t *ptr = static_cast<const uint8_t *>(data);
    decryptedData.resize(len);

    if (m_context->_key_size == KeySize::AES_128) {
        aes128_set_encrypt_key(&m_context->_ctx._aes128, m_context->_user_key);
        if (m_context->_encrypt_mode == EncryptMode::ECB) {
            size_t offset = 0;
            size_t outputOffset = 0;
            while (offset < len) {
                nettle_aes128_decrypt(&m_context->_ctx._aes128, AES_BLOCK_SIZE, &decryptedData[outputOffset], ptr + offset);
                offset += AES_BLOCK_SIZE;
                outputOffset += AES_BLOCK_SIZE;
            }
        } else if (m_context->_encrypt_mode == EncryptMode::CBC) {
            uint8_t temporaryIV[CBC_IV_SIZE];
            memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);

            size_t blockSize = len;
            nettle_cbc_encrypt(&m_context->_ctx._aes128, (nettle_cipher_func*)nettle_aes128_encrypt, 
                CBC_IV_SIZE, temporaryIV, blockSize, decryptedData.data(), ptr);
        }
    } else if (m_context->_key_size == KeySize::AES_256) {
        aes256_set_encrypt_key(&m_context->_ctx._aes256, m_context->_user_key);
        if (m_context->_encrypt_mode == EncryptMode::ECB) {
            size_t offset = 0;
            size_t outputOffset = 0;
            while (offset < len) {
                nettle_aes256_decrypt(&m_context->_ctx._aes256, AES_BLOCK_SIZE, &decryptedData[outputOffset], ptr + offset);
                offset += AES_BLOCK_SIZE;
                outputOffset += AES_BLOCK_SIZE;
            }
        } else if (m_context->_encrypt_mode == EncryptMode::CBC) {
            uint8_t temporaryIV[CBC_IV_SIZE];
            memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);

            size_t blockSize = len;
            nettle_cbc_encrypt(&m_context->_ctx._aes256, (nettle_cipher_func*)nettle_aes256_decrypt, 
                CBC_IV_SIZE, temporaryIV, blockSize, decryptedData.data(), ptr);
        }
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

#endif // HAVE_GNUTLS
