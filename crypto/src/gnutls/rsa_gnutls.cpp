/*************************************************************************
    > File Name: rsa_gnutls.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月14日 星期一 17时37分57秒
 ************************************************************************/

#include "rsa.h"

#include <string.h>

#include "rsa_error.h"

#if defined(HAVE_GNUTLS)
#include <nettle/rsa.h>
#include <nettle/yarrow.h>
#include <nettle/buffer.h>

#ifndef RSA_PADDING
#define RSA_PADDING MBEDTLS_RSA_PKCS_V15 // PKCS1_PADDING
#endif // RSA_PADDING

namespace eular {
namespace crypto {
class RSAContex
{
public:
    rsa_public_key*     _publicRsaKey{};
    rsa_private_key*    _privateRsaKey{};
    std::string         _publicKey;
    std::string         _privateKey;

    RSAContex() {
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
    uint8_t seed[32];
    struct yarrow256_ctx yarrowCtx;

    rsa_public_key_init(&pubKey);
    rsa_private_key_init(&privKey);
    yarrow256_seed(&yarrowCtx, 32, seed);

    struct nettle_buffer pubKeyBuf;
    struct nettle_buffer privKeyBuf;
    nettle_buffer_init(&pubKeyBuf);
    nettle_buffer_init(&privKeyBuf);

    // 生成密钥（2048位）
    if (!nettle_rsa_generate_keypair(&pubKey, &privKey, &yarrowCtx, 
                                      (nettle_random_func*)yarrow256_random, 
                                      NULL, NULL, keyBits, 0)) {
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
    (void)publicKey;
    (void)privateKey;

    return RSA_ERROR_NOT_INITIALIZED;
}

int32_t Rsa::publicEncrypt(const void *data, size_t dataSize, std::vector<uint8_t> &encryptedData)
{
    (void)data;
    (void)dataSize;
    (void)encryptedData;

    return RSA_ERROR_NOT_INITIALIZED;
}

int32_t Rsa::publicDecrypt(const void *data, size_t dataSize, std::vector<uint8_t> &decryptedData)
{
    (void)data;
    (void)dataSize;
    (void)decryptedData;

    return RSA_ERROR_NOT_INITIALIZED;
}

int32_t Rsa::publicDecrypt(const void *data, size_t dataSize, std::string &decryptedData)
{
    (void)data;
    (void)dataSize;
    (void)decryptedData;

    return RSA_ERROR_NOT_INITIALIZED;
}

int32_t Rsa::privateEncrypt(const void *data, size_t dataSize, std::vector<uint8_t> &encryptedData)
{
    (void)data;
    (void)dataSize;
    (void)encryptedData;

    return RSA_ERROR_NOT_INITIALIZED;
}

int32_t Rsa::privateDecrypt(const void *data, size_t dataSize, std::vector<uint8_t> &decryptedData)
{
    (void)data;
    (void)dataSize;
    (void)decryptedData;

    return RSA_ERROR_NOT_INITIALIZED;
}

int32_t Rsa::privateDecrypt(const void *data, size_t dataSize, std::string &decryptedData)
{
    (void)data;
    (void)dataSize;
    (void)decryptedData;

    return RSA_ERROR_NOT_INITIALIZED;
}

int32_t Rsa::rsaPaddingSize(const void *key, int32_t &keySize) const
{
    (void)key;
    (void)keySize;

    return RSA_ERROR_NOT_INITIALIZED;
}

} // namespace crypto
} // namespace eular

#endif // HAVE_GNUTLS
