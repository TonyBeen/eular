/*************************************************************************
    > File Name: rsa_gnutls.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月14日 星期一 17时37分57秒
 ************************************************************************/

#include "rsa.h"

#include <string.h>
#include <cassert>
#include <random>

#include "rsa_error.h"

#if defined(HAVE_GNUTLS)
#include <nettle/rsa.h>
#include <nettle/yarrow.h>
#include <nettle/buffer.h>

#define EXPONENT    65537

namespace eular {
namespace crypto {
class RSAContex
{
public:
    rsa_public_key      _publicRSACtx;
    rsa_private_key     _privateRSACtx;
    rsa_public_key*     _publicRsaKey{};
    rsa_private_key*    _privateRsaKey{};
    std::string         _publicKey;
    std::string         _privateKey;
    int32_t             _md;
    yarrow256_ctx       _ctrDrbg;

    RSAContex() :
        _md(Rsa::MT_SHA256)
    {
        yarrow256_init(&_ctrDrbg, 0, nullptr);
    }

    ~RSAContex() {
        clean();
    }

    void clean()
    {
        if (_publicRsaKey) {
            rsa_public_key_clear(_publicRsaKey);
            _publicRsaKey = nullptr;
        }

        if (_privateRsaKey) {
            rsa_private_key_clear(_privateRsaKey);
            _privateRsaKey = nullptr;
        }

        _publicKey.clear();
        _privateKey.clear();
    }
};


Rsa::Rsa()
{
}

Rsa::Rsa(const std::string &publicKey, const std::string &privateKey)
{
    initRSAKey(publicKey, privateKey);
}

Rsa::~Rsa()
{
    m_context.reset();
}

int32_t Rsa::GenerateRSAKey(std::string &publicKey, std::string &privateKey, int32_t keyBits)
{
    int32_t status = RSA_ERROR_NONE;

    struct rsa_public_key pubKey;
    struct rsa_private_key privKey;
    struct yarrow256_ctx yarrowCtx;
    uint8_t seed[20];
    std::random_device rd;
    std::mt19937_64 engine(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    for (size_t i = 0; i < sizeof(seed); ++i) {
        seed[i] = dist(engine);
    }

    rsa_public_key_init(&pubKey);
    rsa_private_key_init(&privKey);
    nettle_yarrow256_init(&yarrowCtx, 0, nullptr);
    nettle_yarrow256_seed(&yarrowCtx, sizeof(seed), seed);

    struct nettle_buffer pubKeyBuf;
    struct nettle_buffer privKeyBuf;
    nettle_buffer_init(&pubKeyBuf);
    nettle_buffer_init(&privKeyBuf);

    mpz_set_ui(pubKey.e, EXPONENT);
    if (!nettle_rsa_generate_keypair(&pubKey, &privKey,
                                     &yarrowCtx, (nettle_random_func*)yarrow256_random, 
                                     NULL, NULL, keyBits, EXPONENT)) {
        return RSA_ERROR_GENERATE_KEY;
    }

    do {
        if (!rsa_keypair_to_sexp(&pubKeyBuf, "rsa-pkcs1-sha1", &pubKey, NULL)) {
            status = -errno;
            break;
        }
        if (!rsa_keypair_to_sexp(&privKeyBuf, "rsa-pkcs1-sha1", &pubKey, &privKey)) {
            status = -errno;
            break;
        }

        publicKey = std::move(std::string((const char *)pubKeyBuf.contents, pubKeyBuf.size));
        privateKey = std::move(std::string((const char *)privKeyBuf.contents, privKeyBuf.size));
    } while (false);

    nettle_buffer_clear(&pubKeyBuf);
    nettle_buffer_clear(&privKeyBuf);
    rsa_public_key_clear(&pubKey);
    rsa_private_key_clear(&privKey);
    return status;
}

std::string Rsa::Status2Msg(int32_t status)
{
    return std::string();
}

int32_t Rsa::initRSAKey(const std::string &publicKey, const std::string &privateKey)
{
    int32_t status = RSA_ERROR_NONE;
    if (publicKey.empty() || privateKey.empty()) {
        return RSA_ERROR_INVALID_KEY; // Invalid key input
    }

    if (!m_context) {
        m_context = std::unique_ptr<RSAContex>(new RSAContex());
    }

    // 清除之前的密钥
    m_context->clean();

    do {
        nettle_rsa_public_key_init(&m_context->_publicRSACtx);
        nettle_rsa_private_key_init(&m_context->_privateRSACtx);

        // 解析公钥
        if (!nettle_rsa_keypair_from_sexp(&m_context->_publicRSACtx, nullptr, 0,
                                           publicKey.size(), (const uint8_t *)publicKey.data())) {
            status = RSA_ERROR_PUBLIC_KEY_PARSE;
            break;
        }
        m_context->_publicRsaKey = &m_context->_publicRSACtx;
        m_context->_publicKey = publicKey;

        // 解析私钥
        if (!nettle_rsa_keypair_from_sexp(&m_context->_publicRSACtx, &m_context->_privateRSACtx, 0,
                                           privateKey.size(), (const uint8_t *)privateKey.data())) {
            status = RSA_ERROR_PRIVATE_KEY_PARSE;
            break;
        }
        m_context->_privateRsaKey = &m_context->_privateRSACtx;
        m_context->_privateKey = privateKey;

        return status;
    } while (false);

    m_context->clean();
    return status;
}

void eular::crypto::Rsa::setHashMode(HashMethod md)
{
    if (m_context) {
        switch (md) {
        case MT_MD5:
        case MT_SHA1:
        case MT_SHA256:
        case MT_SHA512:
            m_context->_md = md;
            break;
        default:
            break;
        }
    }
}

int32_t Rsa::publicEncrypt(const void *data, size_t dataSize, std::vector<uint8_t> &encryptedData)
{
    int32_t status = RSA_ERROR_NONE;
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
    encryptedData.reserve((dataSize + keySize - 1) / keySize * keySize);
    std::vector<uint8_t> blockVec(keySize);
    blockVec.resize(keySize);
    mpz_t mpz;
    mpz_init(mpz);
    for (size_t i = 0; i < dataSize; i += blockSize) {
        size_t blockLen = MIN((size_t)blockSize, dataSize - i);
        status = nettle_rsa_encrypt(m_context->_publicRsaKey, &m_context->_ctrDrbg, (nettle_random_func *)yarrow256_random, blockLen, &ptr[i], mpz);
        if (status == 0) {
            mpz_clear(mpz);
            return RSA_ERROR_ENCRYPTION_FAILED;
        }

        uint32_t size = (uint32_t)nettle_mpz_sizeinbase_256_u(mpz);
        assert(size == (uint32_t)keySize);
        blockVec.resize(size);
        nettle_mpz_get_str_256(size, blockVec.data(), mpz);

        encryptedData.insert(encryptedData.end(), blockVec.begin(), blockVec.end());
    }

    mpz_clear(mpz);
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
    mpz_t mpz;
    nettle_mpz_init_set_str_256_u(mpz, signatureSize, signaturePtr);
    int32_t status = 0;
    switch (m_context->_md) {
    case MT_MD5:
    if (hashVec.size() == MD5_DIGEST_SIZE) {
        status = nettle_rsa_md5_verify_digest(m_context->_publicRsaKey, hashVec.data(), mpz);
    }
        break;
    case MT_SHA1:
    if (hashVec.size() == SHA1_DIGEST_SIZE) {
        status = nettle_rsa_sha1_verify_digest(m_context->_publicRsaKey, hashVec.data(), mpz);
    }
        break;
    case MT_SHA256:
    if (hashVec.size() == SHA256_DIGEST_SIZE) {
        status = nettle_rsa_sha256_verify_digest(m_context->_publicRsaKey, hashVec.data(), mpz);
    }
        break;
    case MT_SHA512:
    if (hashVec.size() == SHA512_DIGEST_SIZE) {
        status = nettle_rsa_sha512_verify_digest(m_context->_publicRsaKey, hashVec.data(), mpz);
    }
        break;
    default:
        break;
    }

    mpz_clear(mpz);
    return status == 1 ? RSA_ERROR_NONE : RSA_ERROR_VERIFY_SIGNATURE;
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
    int32_t status = 0;
    mpz_t mpz;
    nettle_mpz_init_set_str_256_u(mpz, signatureVec.size(), signatureVec.data());
    switch (m_context->_md) {
    case MT_MD5:
        status = nettle_rsa_md5_sign_digest(m_context->_privateRsaKey, hashVec.data(), mpz);
        break;
    case MT_SHA1:
        status = nettle_rsa_sha1_sign_digest(m_context->_privateRsaKey, hashVec.data(), mpz);
        break;
    case MT_SHA256:
        status = nettle_rsa_sha256_sign_digest(m_context->_privateRsaKey, hashVec.data(), mpz);
        break;
    case MT_SHA512:
        status = nettle_rsa_sha512_sign_digest(m_context->_privateRsaKey, hashVec.data(), mpz);
        break;
    default:
        break;
    }
    mpz_clear(mpz);
    return status == 1 ? RSA_ERROR_NONE : RSA_ERROR_SIGNATURE_FAILED;
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
    int32_t blockSize = rsaPaddingSize(m_context->_publicRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }
    if (dataSize % keySize != 0) {
        return RSA_ERROR_INVALID_PARAMETER; // Invalid data input
    }

    const uint8_t *ptr = (const uint8_t *)data;
    mpz_t mpz;
    decryptedData.reserve(dataSize);
    std::vector<uint8_t> blockVec(keySize);
    blockVec.resize(keySize);
    for (size_t i = 0; i < dataSize; i += keySize) {
        size_t blockLen = blockVec.capacity();
        nettle_mpz_init_set_str_256_u(mpz, keySize, ptr + i);
        int32_t status = rsa_decrypt(m_context->_privateRsaKey, &blockLen, blockVec.data(), mpz);
        if (status < 0) {
            mpz_clear(mpz);
            return status;
        }

        decryptedData.insert(decryptedData.end(), blockVec.begin(), blockVec.begin() + blockLen);
    }

    mpz_clear(mpz);
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
    int32_t blockSize = rsaPaddingSize(m_context->_publicRsaKey, keySize);
    if (blockSize < 0) {
        return blockSize; // Error in calculating padding size
    }

    const uint8_t *ptr = (const uint8_t *)data;
    mpz_t mpz;
    decryptedData.reserve(dataSize);
    std::vector<uint8_t> blockVec(keySize);
    blockVec.resize(keySize);
    for (size_t i = 0; i < dataSize; i += keySize) {
        size_t blockLen = blockVec.capacity();
        nettle_mpz_init_set_str_256_u(mpz, keySize, ptr + i);
        int32_t status = rsa_decrypt(m_context->_privateRsaKey, &blockLen, blockVec.data(), mpz);
        if (status < 0) {
            mpz_clear(mpz);
            return status;
        }

        decryptedData.append((const char *)blockVec.data(), blockLen);
    }

    mpz_clear(mpz);
    return RSA_ERROR_NONE;
}

int32_t Rsa::rsaPaddingSize(const void *key, int32_t &keySize) const
{
    rsa_public_key *rsaKey = (rsa_public_key *)key;
    size_t bits = mpz_sizeinbase(rsaKey->n, 2);
    keySize = (bits + 7) / 8;

    // nettle 只支持 PKCS1_PADDING
    return keySize - 11;
}

} // namespace crypto
} // namespace eular

#endif // HAVE_GNUTLS
