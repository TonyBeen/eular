/*************************************************************************
    > File Name: aes_openssl.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月09日 星期三 10时49分17秒
 ************************************************************************/

#include "aes.h"

#include <assert.h>

#if defined(HAVE_OPENSSL)
#undef AES_BLOCK_SIZE
#include <openssl/aes.h>

namespace eular {
namespace crypto {
class AESContext {
public:
    AES_KEY     _ctx;
    uint8_t     _user_key[AES::KeySize::AES_256];
    uint8_t     _key_size;
    uint8_t     _encrypt_mode;
    uint8_t     _iv[CBC_IV_SIZE];
};

AES::AES() :
    m_context(new AESContext())
{
    memset(m_context->_user_key, 0, sizeof(m_context->_user_key)); // Initialize user key to zero
    m_context->_key_size = AES::KeySize::AES_256; // Default to AES-256
    m_context->_encrypt_mode = EncryptMode::ECB; // Default to ECB mode
    memset(m_context->_iv, 0, CBC_IV_SIZE); // Initialize IV to zero
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
    AES_set_encrypt_key(m_context->_user_key, m_context->_key_size * 8, &m_context->_ctx);

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

            AES_ecb_encrypt(ptr + offset, encryptedData.data() + outputOffset, &m_context->_ctx, AES_ENCRYPT);
            offset += AES_BLOCK_SIZE;
            outputOffset += AES_BLOCK_SIZE;
        }

        // Handle padding
        uint8_t temporaryBuffer[AES_BLOCK_SIZE] = {0};
        int64_t remaining = (int64_t)len - (int64_t)offset;
        memcpy(temporaryBuffer, ptr + offset, remaining);
        memset(temporaryBuffer + remaining, (uint8_t)paddingSize, paddingSize);
        AES_ecb_encrypt(temporaryBuffer, encryptedData.data() + outputOffset, &m_context->_ctx, AES_ENCRYPT);
    } else if (m_context->_encrypt_mode == EncryptMode::CBC) {
        uint8_t temporaryIV[CBC_IV_SIZE];
        memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);

        size_t blockSize = len / AES_BLOCK_SIZE * AES_BLOCK_SIZE;
        AES_cbc_encrypt(ptr + offset, encryptedData.data() + outputOffset, blockSize, &m_context->_ctx, temporaryIV, AES_ENCRYPT);
        offset += blockSize;
        outputOffset += blockSize;

        // Handle padding
        memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);
        uint8_t temporaryBuffer[AES_BLOCK_SIZE] = {0};
        int64_t remaining = (int64_t)len - (int64_t)offset;
        memcpy(temporaryBuffer, ptr + offset, remaining);
        memset(temporaryBuffer + remaining, (uint8_t)paddingSize, paddingSize);
        AES_cbc_encrypt(temporaryBuffer, encryptedData.data() + outputOffset, AES_BLOCK_SIZE, &m_context->_ctx, temporaryIV, AES_ENCRYPT);
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
    AES_set_decrypt_key(m_context->_user_key, m_context->_key_size * 8, &m_context->_ctx);
    decryptedData.resize(len);

    if (m_context->_encrypt_mode == EncryptMode::ECB) {
        size_t offset = 0;
        size_t outputOffset = 0;
        while (offset < len) {
            AES_ecb_encrypt(ptr + offset, decryptedData.data() + outputOffset, &m_context->_ctx, AES_DECRYPT);
            offset += AES_BLOCK_SIZE;
            outputOffset += AES_BLOCK_SIZE;
        }
    } else if (m_context->_encrypt_mode == EncryptMode::CBC) {
        uint8_t temporaryIV[CBC_IV_SIZE];
        memcpy(temporaryIV, m_context->_iv, CBC_IV_SIZE);

        size_t blockSize = len;
        AES_cbc_encrypt(ptr, decryptedData.data(), blockSize, &m_context->_ctx, temporaryIV, AES_DECRYPT);
    }

    // Remove padding
    uint8_t paddingSize = decryptedData.back();
    assert(paddingSize > 0 && paddingSize <= AES_BLOCK_SIZE); // Ensure valid padding size
    if (paddingSize > 0 && paddingSize <= AES_BLOCK_SIZE) {
        decryptedData.resize(decryptedData.size() - paddingSize);
    }

    return decryptedData;
}

} // namespace crypto
} // namespace eular

#endif