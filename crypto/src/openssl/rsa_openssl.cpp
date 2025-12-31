/*************************************************************************
    > File Name: rsa_openssl.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月11日 星期五 17时35分40秒
 ************************************************************************/

#include "rsa.h"

#include <assert.h>

#include "rsa_error.h"

#if defined(HAVE_OPENSSL)
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

#ifndef RSA_PADDING
#define RSA_PADDING RSA_PKCS1_PADDING
#endif // RSA_PADDING

namespace eular {
namespace crypto {
class RSAContex
{
public:
    RSA*            _publicRsaKey{nullptr};
    RSA*            _privateRsaKey{nullptr};
    std::string     _publicKey;
    std::string     _privateKey;
    int32_t         _md;

    RSAContex() :
        _md(NID_sha256)
    {
    }

    ~RSAContex()
    {
        clean();
    }

    void clean()
    {
        if (_publicRsaKey) {
            RSA_free(_publicRsaKey);
            _publicRsaKey = nullptr;
        }

        if (_privateRsaKey) {
            RSA_free(_privateRsaKey);
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
    RSA *keyPair = RSA_generate_key(keyBits, RSA_F4, nullptr, nullptr);
    if (!keyPair) {
        status = (int32_t)ERR_get_error();
        return status; // Key generation failed
    }

    BIO *privateBIO = BIO_new(BIO_s_mem());
    if (!privateBIO) {
        RSA_free(keyPair);
        return (int32_t)ERR_get_error(); // Memory allocation failed for private BIO
    }
    BIO *publicBIO = BIO_new(BIO_s_mem());
    if (!publicBIO) {
        BIO_free(privateBIO);
        RSA_free(keyPair);
        return (int32_t)ERR_get_error(); // Memory allocation failed for public BIO
    }

    do {
        // Write the public key to the BIO
        if (!PEM_write_bio_RSA_PUBKEY(publicBIO, keyPair)) {
            status = (int32_t)ERR_get_error();
            break; // Public key write failed
        }

        // Write the private key to the BIO
        if (!PEM_write_bio_RSAPrivateKey(privateBIO, keyPair, nullptr, nullptr, 0, nullptr, nullptr)) {
            status = (int32_t)ERR_get_error();
            break; // Private key write failed
        }

        // Get the length of the keys
        privateKeyLen = BIO_pending(privateBIO);
        privateKey.resize(privateKeyLen);
        publicKeyLen = BIO_pending(publicBIO);
        publicKey.resize(publicKeyLen);

        BIO_read(privateBIO, &privateKey[0], privateKeyLen);
        BIO_read(publicBIO, &publicKey[0], publicKeyLen);
    } while (false);

    BIO_free_all(privateBIO);
    BIO_free_all(publicBIO);
    RSA_free(keyPair);
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
    ERR_error_string_n(static_cast<unsigned long>(status), errBuf, sizeof(errBuf));
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

    BIO* bio = nullptr;
    do {
        m_context->_publicRsaKey = RSA_new();
        if (!m_context->_publicRsaKey) {
            status = (int32_t)ERR_get_error();
            break; // Failed to create RSA structure
        }

        m_context->_privateRsaKey = RSA_new();
        if (!m_context->_privateRsaKey) {
            status = (int32_t)ERR_get_error();
            break; // Failed to create RSA structure
        }

        bio = BIO_new_mem_buf(publicKey.data(), publicKey.size());
        if (!bio) {
            status = (int32_t)ERR_get_error();
            break; // Memory allocation failed for BIO
        }
        // Read the public key from the BIO
        m_context->_publicRsaKey = PEM_read_bio_RSA_PUBKEY(bio, &m_context->_publicRsaKey, nullptr, nullptr);
        if (!m_context->_publicRsaKey) {
            status = (int32_t)ERR_get_error();
            break; // Failed to read public key
        }
        BIO_free(bio);

        bio = BIO_new_mem_buf(privateKey.data(), privateKey.size());
        if (!bio) {
            status = (int32_t)ERR_get_error();
            break; // Memory allocation failed for BIO
        }
        // Read the private key from the BIO
        m_context->_privateRsaKey = PEM_read_bio_RSAPrivateKey(bio, &m_context->_privateRsaKey, nullptr, nullptr);
        if (!m_context->_privateRsaKey) {
            status = (int32_t)ERR_get_error();
            break; // Failed to read private key
        }
        // Successfully initialized both keys
        BIO_free(bio);
        m_context->_publicKey = publicKey;
        m_context->_privateKey = privateKey;
        return status; // Success
    } while (false);

    if (bio != nullptr) {
        BIO_free(bio);
    }

    m_context->clean();
    return status;
}

void Rsa::setHashMode(HashMethod md)
{
    if (m_context) {
        switch (md) {
        case MT_MD5:
            m_context->_md = NID_md5;
            break;
        case MT_SHA1:
            m_context->_md = NID_sha1;
            break;
        case MT_SHA256:
            m_context->_md = NID_sha256;
            break;
        case MT_SHA512:
            m_context->_md = NID_sha512;
            break;
        default:
            break;
        }
    }
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
        size_t blockLen = MIN((size_t)blockSize, dataSize - i);
        int32_t encryptedSize = RSA_public_encrypt(static_cast<int32_t>(blockLen), &ptr[i],
                                                   blockVec.data(), m_context->_publicRsaKey, RSA_PADDING);
        if (encryptedSize < 0) {
            return (int32_t)ERR_get_error();
        }
        encryptedData.insert(encryptedData.end(), blockVec.begin(), blockVec.begin() + encryptedSize);
    }

    return RSA_ERROR_NONE;
}

int32_t Rsa::verifySignature(const void *signatureData, size_t signatureSize, const std::vector<uint8_t> &hashVec)
{
    if (m_context == nullptr || m_context->_publicRsaKey == nullptr) {
        return RSA_ERROR_NOT_INITIALIZED; // RSA context or public key not initialized
    }
    if (signatureData == nullptr || signatureSize == 0) {
        return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
    }
    int32_t keySize = 0;
    int32_t blockSize = rsaPaddingSize(m_context->_publicRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }
    if (signatureSize != (uint32_t)keySize) {
        return RSA_ERROR_INVALID_PARAMETER;
    }

    const uint8_t *signaturePtr = (const uint8_t *)signatureData;
    int32_t status = RSA_verify(m_context->_md, hashVec.data(), hashVec.size(),
                                signaturePtr, signatureSize,
                                m_context->_publicRsaKey);
    return status == 1 ? RSA_ERROR_NONE : (int32_t)ERR_get_error();
}

int32_t Rsa::sign(const std::vector<uint8_t> &hashVec, std::vector<uint8_t> &signatureVec)
{
    if (m_context == nullptr || m_context->_privateRsaKey == nullptr) {
        return RSA_ERROR_NOT_INITIALIZED; // RSA context or public key not initialized
    }

    int32_t keySize = 0;
    int32_t blockSize = rsaPaddingSize(m_context->_publicRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }

    signatureVec.resize(keySize);
    uint32_t siglen = 0;
    int32_t status = RSA_sign(m_context->_md,
                              hashVec.data(), hashVec.size(),
                              signatureVec.data(), &siglen,
                              m_context->_privateRsaKey);
    if (status != 1) {
        return (int32_t)ERR_get_error();
    }
    assert(siglen == (uint32_t)keySize);
    signatureVec.resize(siglen);
    return RSA_ERROR_NONE;
}

// int32_t Rsa::publicDecrypt(const void *data, size_t dataSize, std::vector<uint8_t> &decryptedData)
// {
//     if (m_context == nullptr || m_context->_publicRsaKey == nullptr) {
//         return RSA_ERROR_NOT_INITIALIZED; // RSA context or public key not initialized
//     }
//     if (data == nullptr || dataSize == 0) {
//         return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
//     }
//     int32_t keySize = 0;
//     int32_t blockSize = rsaPaddingSize(m_context->_publicRsaKey, keySize);
//     if (blockSize < 0) {
//         return blockSize; // Error in calculating padding size
//     }
//     const uint8_t *ptr = (const uint8_t *)data;
//     decryptedData.reserve(dataSize);
//     std::vector<uint8_t> blockVec(keySize);
//     for (size_t i = 0; i < dataSize; i += keySize) {
//         size_t blockLen = MIN((size_t)keySize, dataSize - i);
//         int32_t decryptedSize = RSA_public_decrypt(static_cast<int32_t>(blockLen), &ptr[i],
//                                                    blockVec.data(), m_context->_publicRsaKey, RSA_PADDING);
//         if (decryptedSize < 0) {
//             return (int32_t)ERR_get_error();
//         }
//         decryptedData.insert(decryptedData.end(), blockVec.begin(), blockVec.begin() + decryptedSize);
//     }

//     return RSA_ERROR_NONE;
// }

// int32_t Rsa::publicDecrypt(const void *data, size_t dataSize, std::string &decryptedData)
// {
//     if (m_context == nullptr || m_context->_publicRsaKey == nullptr) {
//         return RSA_ERROR_NOT_INITIALIZED; // RSA context or public key not initialized
//     }
//     if (data == nullptr || dataSize == 0) {
//         return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
//     }
//     int32_t keySize = 0;
//     int32_t blockSize = rsaPaddingSize(m_context->_publicRsaKey, keySize);
//     if (blockSize < 0) {
//         return blockSize; // Error in calculating padding size
//     }

//     const uint8_t *ptr = (const uint8_t *)data;
//     decryptedData.reserve(dataSize);
//     std::vector<uint8_t> blockVec(keySize);
//     for (size_t i = 0; i < dataSize; i += keySize) {
//         size_t blockLen = MIN((size_t)keySize, dataSize - i);
//         int32_t decryptedSize = RSA_public_decrypt(static_cast<int32_t>(blockLen), &ptr[i],
//                                                    blockVec.data(), m_context->_publicRsaKey, RSA_PADDING);
//         if (decryptedSize < 0) {
//             return (int32_t)ERR_get_error();
//         }
//         decryptedData.append(reinterpret_cast<const char *>(blockVec.data()), decryptedSize);
//     }

//     return RSA_ERROR_NONE;
// }

// int32_t Rsa::privateEncrypt(const void *data, size_t dataSize, std::vector<uint8_t> &encryptedData)
// {
//     if (m_context == nullptr || m_context->_privateRsaKey == nullptr) {
//         return RSA_ERROR_NOT_INITIALIZED; // RSA context or private key not initialized
//     }
//     if (data == nullptr || dataSize == 0) {
//         return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
//     }
//     int32_t keySize = 0;
//     int32_t blockSize = rsaPaddingSize(m_context->_privateRsaKey, keySize);
//     if (blockSize < 0) {
//         return blockSize; // Error in calculating padding size
//     }

//     const uint8_t *ptr = (const uint8_t *)data;
//     encryptedData.reserve(dataSize);
//     std::vector<uint8_t> blockVec(keySize);
//     for (size_t i = 0; i < dataSize; i += blockSize) {
//         size_t blockLen = MIN((size_t)blockSize, dataSize - i);
//         int32_t encryptedSize = RSA_private_encrypt(static_cast<int32_t>(blockLen), &ptr[i],
//                                                     blockVec.data(), m_context->_privateRsaKey, RSA_PADDING);
//         if (encryptedSize < 0) {
//             return (int32_t)ERR_get_error();
//         }
//         encryptedData.insert(encryptedData.end(), blockVec.begin(), blockVec.begin() + encryptedSize);
//     }

//     return RSA_ERROR_NONE;
// }

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
        size_t blockLen = MIN((size_t)keySize, dataSize - i);
        int32_t decryptedSize = RSA_private_decrypt(static_cast<int32_t>(blockLen), &ptr[i],
                                                    blockVec.data(), m_context->_privateRsaKey, RSA_PADDING);
        if (decryptedSize < 0) {
            return (int32_t)ERR_get_error();
        }
        decryptedData.insert(decryptedData.end(), blockVec.begin(), blockVec.begin() + decryptedSize);
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
        size_t blockLen = MIN((size_t)keySize, dataSize - i);
        int32_t decryptedSize = RSA_private_decrypt(static_cast<int32_t>(blockLen), &ptr[i],
                                                    blockVec.data(), m_context->_privateRsaKey, RSA_PADDING);
        if (decryptedSize < 0) {
            return (int32_t)ERR_get_error();
        }
        decryptedData.append(reinterpret_cast<const char *>(blockVec.data()), decryptedSize);
    }

    return RSA_ERROR_NONE;
}

int32_t Rsa::rsaPaddingSize(const void *key, int32_t &keySize) const
{
    RSA *rsaKey = (RSA *)key;
    keySize = RSA_size(rsaKey);
    if (keySize <= 0) {
        return RSA_ERROR_INVALID_RSA_SIZE; // Invalid RSA size
    }

    int32_t blockSize = keySize;
    switch (RSA_PADDING) {
    case RSA_PKCS1_PADDING:
        blockSize = keySize - 11;
        break;
    case RSA_NO_PADDING:
        blockSize = keySize;
        break;
    case RSA_X931_PADDING:
        blockSize = keySize - 2;
        break;
    case RSA_PKCS1_OAEP_PADDING:
        blockSize = keySize - 42;
        break;
    default:
        return RSA_ERROR_INVALID_PADDING; // Unsupported padding type
    }

    return blockSize;
}
}
}

#endif // defined(HAVE_OPENSSL)
