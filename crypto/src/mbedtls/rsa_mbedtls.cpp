/*************************************************************************
    > File Name: rsa_mbedtls.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月14日 星期一 15时46分44秒
 ************************************************************************/

#include "rsa.h"

#include <string.h>

#include "rsa_error.h"

#if defined(HAVE_MBEDTLS)
#define MBEDTLS_RSA_C
#include <mbedtls/rsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>

#define EXPONENT    65537
#ifndef RSA_PADDING
#define RSA_PADDING MBEDTLS_RSA_PKCS_V15 // PKCS1_PADDING
#endif // RSA_PADDING

namespace eular {
namespace crypto {
class RSAContex
{
public:
    mbedtls_pk_context          _publicPKContext;
    mbedtls_pk_context          _privatePKContext;
    mbedtls_ctr_drbg_context    _ctrDrbg;
    mbedtls_rsa_context*        _publicRsaKey{};
    mbedtls_rsa_context*        _privateRsaKey{};
    std::string                 _publicKey;
    std::string                 _privateKey;

    RSAContex() {
        mbedtls_ctr_drbg_init(&_ctrDrbg);
    }

    ~RSAContex() {
        mbedtls_ctr_drbg_free(&_ctrDrbg);
        clean();
    }

    void clean()
    {
        if (_publicRsaKey) {
            mbedtls_pk_free(&_publicPKContext);
            _publicRsaKey = nullptr;
        }

        if (_privateRsaKey) {
            mbedtls_pk_free(&_privatePKContext);
            _privateRsaKey = nullptr;
        }

        _publicKey.clear();
        _privateKey.clear();
    }
};


Rsa::Rsa()
{
}

Rsa::Rsa(const std::string &publicKey, const std::string &privateKey) :
    m_context(new RSAContex())
{
    initRSAKey(publicKey, privateKey);
}

Rsa::~Rsa()
{
    m_context.reset();
}

int32_t Rsa::GenerateRSAKey(std::string &publicKey, std::string &privateKey, int32_t keyBits)
{
    size_t privateKeyLen = 0;
    size_t publicKeyLen = 0;
    int32_t status = RSA_ERROR_NONE;

    mbedtls_pk_context          pk;
    mbedtls_entropy_context     entropy;
    mbedtls_ctr_drbg_context    ctr_drbg;
    const char *pers = "rsa_gen_key";

    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    do {
        status = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                (const uint8_t *)pers, strlen(pers));
        if (status != 0) {
            break; // Failed to seed the random number generator
        }
        status = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
        if (status != 0) {
            break; // Failed to setup the PK context
        }
        status = mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), mbedtls_ctr_drbg_random, &ctr_drbg, keyBits, EXPONENT);
        if (status != 0) {
            break; // Failed to generate RSA key
        }

        privateKey.resize(keyBits);
        status = mbedtls_pk_write_key_pem(&pk, (uint8_t *)privateKey.data(), privateKey.size());
        if (status != 0) {
            privateKey.clear(); // Clear private key on failure
            publicKey.clear(); // Clear public key on failure
            break; // Failed to write private key to PEM format
        }
        privateKeyLen = strlen(privateKey.c_str());
        privateKey.resize(privateKeyLen); // Resize to actual length

        publicKey.resize(keyBits);
        status = mbedtls_pk_write_pubkey_pem(&pk, (uint8_t *)publicKey.data(), publicKey.size());
        if (status != 0) {
            privateKey.clear(); // Clear private key on failure
            publicKey.clear(); // Clear public key on failure
            break; // Failed to write public key to PEM format
        }
        publicKeyLen = strlen(publicKey.c_str());
        publicKey.resize(publicKeyLen); // Resize to actual length
    } while (false);

    mbedtls_pk_free(&pk);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    return status; // Success
}

std::string Rsa::Status2Msg(int32_t status)
{
    const char *msg = NULL;
    RSA_ERROR_MSG(status, msg);
    if (msg != NULL) {
        return std::string(msg);
    }

    char errBuf[512] = {0};
    mbedtls_strerror(status, errBuf, sizeof(errBuf));
    return errBuf;
}

int32_t Rsa::initRSAKey(const std::string &publicKey, const std::string &privateKey)
{
    int32_t status = RSA_ERROR_NONE;
    if (publicKey.empty() || privateKey.empty()) {
        return RSA_ERROR_INVALID_KEY; // Invalid key input
    }

    if (m_context == nullptr) {
        m_context = std::unique_ptr<RSAContex>(new RSAContex());
    }
    m_context->clean();

    do {
        mbedtls_pk_init(&m_context->_publicPKContext);
        status = mbedtls_pk_parse_public_key(&m_context->_publicPKContext, (const uint8_t *)publicKey.data(), publicKey.size() + 1);
        if (status != 0) {
            break; // Failed to parse public key
        }
        m_context->_publicRsaKey = mbedtls_pk_rsa(m_context->_publicPKContext);

        mbedtls_pk_init(&m_context->_privatePKContext);
        status = mbedtls_pk_parse_key(&m_context->_privatePKContext, (const uint8_t *)privateKey.data(), privateKey.size() + 1,
                                       NULL, 0, mbedtls_ctr_drbg_random, NULL);
        if (status != 0) {
            break; // Failed to parse private key
        }
        m_context->_privateRsaKey = mbedtls_pk_rsa(m_context->_privatePKContext);

        mbedtls_rsa_set_padding(m_context->_publicRsaKey, RSA_PADDING, MBEDTLS_MD_NONE);
        mbedtls_rsa_set_padding(m_context->_privateRsaKey, RSA_PADDING, MBEDTLS_MD_NONE);
        return RSA_ERROR_NONE;
    } while (false);

    m_context->clean();
    return status;
}

int32_t Rsa::publicEncrypt(const void *data, size_t dataSize, std::vector<uint8_t> &encryptedData)
{
    if (m_context == nullptr || m_context->_publicRsaKey == nullptr) {
        return RSA_ERROR_NOT_INITIALIZED; // RSA context or public key not initialized
    }
    if (data == nullptr || dataSize == 0) {
        return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
    }
    int32_t keySize = 0;
    int32_t blockSize = rsaPaddingSize(m_context->_publicRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }

    const uint8_t *ptr = (const uint8_t *)data;
    encryptedData.reserve(dataSize);
    std::vector<uint8_t> blockVec(keySize);
    for (size_t i = 0; i < dataSize; i += blockSize) {
        size_t blockLen = MIN(blockSize, dataSize - i);
        int32_t status = mbedtls_rsa_pkcs1_encrypt(m_context->_publicRsaKey, mbedtls_ctr_drbg_random, &m_context->_ctrDrbg,
                                                   static_cast<int32_t>(blockLen), &ptr[i], blockVec.data());
        if (status != 0) {
            return status;
        }
        encryptedData.insert(encryptedData.end(), blockVec.begin(), blockVec.begin() + keySize);
    }

    return RSA_ERROR_NONE;
}

int32_t Rsa::publicDecrypt(const void *data, size_t dataSize, std::vector<uint8_t> &decryptedData)
{
    if (m_context == nullptr || m_context->_publicRsaKey == nullptr) {
        return RSA_ERROR_NOT_INITIALIZED; // RSA context or public key not initialized
    }
    if (data == nullptr || dataSize == 0) {
        return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
    }
    int32_t keySize = 0;
    int32_t blockSize = rsaPaddingSize(m_context->_publicRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }

    const uint8_t *ptr = (const uint8_t *)data;
    decryptedData.reserve(dataSize);
    std::vector<uint8_t> blockVec(keySize);
    for (size_t i = 0; i < dataSize; i += keySize) {
        size_t blockLen = MIN(keySize, dataSize - i);
        int32_t status = mbedtls_rsa_pkcs1_decrypt(m_context->_publicRsaKey, mbedtls_ctr_drbg_random, &m_context->_ctrDrbg,
                                                   &blockLen, &ptr[i], blockVec.data(), keySize);
        if (status != 0) {
            return status;
        }
        decryptedData.insert(decryptedData.end(), blockVec.begin(), blockVec.begin() + blockLen);
    }

    return RSA_ERROR_NONE;
}

int32_t Rsa::publicDecrypt(const void *data, size_t dataSize, std::string &decryptedData)
{
    if (m_context == nullptr || m_context->_publicRsaKey == nullptr) {
        return RSA_ERROR_NOT_INITIALIZED; // RSA context or public key not initialized
    }
    if (data == nullptr || dataSize == 0) {
        return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
    }
    int32_t keySize = 0;
    int32_t blockSize = rsaPaddingSize(m_context->_publicRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }

    const uint8_t *ptr = (const uint8_t *)data;
    decryptedData.reserve(dataSize);
    std::vector<uint8_t> blockVec(keySize);
    for (size_t i = 0; i < dataSize; i += keySize) {
        size_t blockLen = MIN(keySize, dataSize - i);
        int32_t status = mbedtls_rsa_pkcs1_decrypt(m_context->_publicRsaKey, mbedtls_ctr_drbg_random, &m_context->_ctrDrbg,
                                                   &blockLen, &ptr[i], blockVec.data(), keySize);
        if (status != 0) {
            return status;
        }
        decryptedData.append(reinterpret_cast<const char *>(blockVec.data()), blockLen);
    }

    return RSA_ERROR_NONE;
}

int32_t Rsa::privateEncrypt(const void *data, size_t dataSize, std::vector<uint8_t> &encryptedData)
{
    if (m_context == nullptr || m_context->_privateRsaKey == nullptr) {
        return RSA_ERROR_NOT_INITIALIZED; // RSA context or private key not initialized
    }
    if (data == nullptr || dataSize == 0) {
        return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
    }
    int32_t keySize = 0;
    int32_t blockSize = rsaPaddingSize(m_context->_privateRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }

    const uint8_t *ptr = (const uint8_t *)data;
    encryptedData.reserve(dataSize);
    std::vector<uint8_t> blockVec(keySize);
    for (size_t i = 0; i < dataSize; i += blockSize) {
        size_t blockLen = MIN(blockSize, dataSize - i);
        int32_t status = mbedtls_rsa_pkcs1_encrypt(m_context->_privateRsaKey, mbedtls_ctr_drbg_random, &m_context->_ctrDrbg,
                                                   static_cast<int32_t>(blockLen), &ptr[i], blockVec.data());
        if (status != 0) {
            return status;
        }
        encryptedData.insert(encryptedData.end(), blockVec.begin(), blockVec.begin() + keySize);
    }

    return RSA_ERROR_NONE;
}

int32_t Rsa::privateDecrypt(const void *data, size_t dataSize, std::vector<uint8_t> &decryptedData)
{
    if (m_context == nullptr || m_context->_privateRsaKey == nullptr) {
        return RSA_ERROR_NOT_INITIALIZED; // RSA context or private key not initialized
    }
    if (data == nullptr || dataSize == 0) {
        return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
    }
    int32_t keySize = 0;
    int32_t blockSize = rsaPaddingSize(m_context->_privateRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }

    const uint8_t *ptr = (const uint8_t *)data;
    decryptedData.reserve(dataSize);
    std::vector<uint8_t> blockVec(keySize);
    for (size_t i = 0; i < dataSize; i += keySize) {
        size_t blockLen = MIN(keySize, dataSize - i);
        int32_t status = mbedtls_rsa_pkcs1_decrypt(m_context->_privateRsaKey, mbedtls_ctr_drbg_random, &m_context->_ctrDrbg,
                                                   &blockLen, &ptr[i], blockVec.data(), keySize);
        if (status < 0) {
            return status;
        }
        decryptedData.insert(decryptedData.end(), blockVec.begin(), blockVec.begin() + blockLen);
    }

    return RSA_ERROR_NONE;
}

int32_t Rsa::privateDecrypt(const void *data, size_t dataSize, std::string &decryptedData)
{
    if (m_context == nullptr || m_context->_privateRsaKey == nullptr) {
        return RSA_ERROR_NOT_INITIALIZED; // RSA context or private key not initialized
    }
    if (data == nullptr || dataSize == 0) {
        return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
    }
    int32_t keySize = 0;
    int32_t blockSize = rsaPaddingSize(m_context->_privateRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }

    const uint8_t *ptr = (const uint8_t *)data;
    decryptedData.reserve(dataSize);
    std::vector<uint8_t> blockVec(keySize);
    for (size_t i = 0; i < dataSize; i += keySize) {
        size_t blockLen = MIN(keySize, dataSize - i);
        int32_t status = mbedtls_rsa_pkcs1_decrypt(m_context->_privateRsaKey, mbedtls_ctr_drbg_random, &m_context->_ctrDrbg,
                                                   &blockLen, &ptr[i], blockVec.data(), keySize);
        if (status < 0) {
            return status;
        }
        decryptedData.append(reinterpret_cast<const char *>(blockVec.data()), blockLen);
    }

    return RSA_ERROR_NONE;
}

int32_t Rsa::rsaPaddingSize(const void *key, int32_t &keySize) const
{
    mbedtls_rsa_context *rsaKey = (mbedtls_rsa_context *)key;
    keySize = mbedtls_rsa_get_len(rsaKey);
    if (keySize <= 0) {
        return RSA_ERROR_INVALID_RSA_SIZE; // Invalid RSA size
    }

    int32_t blockSize = keySize;
    switch (RSA_PADDING) {
    case MBEDTLS_RSA_PKCS_V15:
        blockSize = keySize - 11;
        break;
    case MBEDTLS_RSA_PKCS_V21: // PKCS1_OAEP_PADDING
        blockSize = keySize - 42;
        break;
    default:
        return RSA_ERROR_INVALID_PADDING; // Unsupported padding type
    }

    return blockSize;
}

} // namespace crypto
} // namespace eular
#endif // HAVE_MBEDTLS