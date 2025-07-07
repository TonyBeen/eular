/*************************************************************************
    > File Name: sha.h
    > Author: hsz
    > Brief:
    > Created Time: Wed 22 Dec 2021 10:08:23 AM CST
 ************************************************************************/

#ifndef __CRYPTO_SHA_H__
#define __CRYPTO_SHA_H__

#include "crypto.h"
#include <utils/exception.h>
#include <utils/errors.h>
#include <openssl/sha.h>

using std::bad_exception;

namespace eular {
class Sha : public CryptoBase
{
public:
    enum class SHA_TYPE {
        SHA256 = 0,
        SHA512 = 1
    };
    static const uint8_t SHA256BUFSIZE = 32;
    static const uint8_t SHA512BUFSIZE = 64;

    Sha(SHA_TYPE whichType); // throw eular::Exception
    virtual ~Sha();

    bool reinit(SHA_TYPE whichType = SHA_TYPE::SHA256); // throw eular::Exception

    virtual int encode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen) override;
    virtual int decode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen) override;

private:
    int EncodeSha256(uint8_t *out, const uint8_t *src, uint32_t len);
    int EncodeSha512(uint8_t *out, const uint8_t *src, uint32_t len);

protected:
    SHA_TYPE   mShaType;
    SHA256_CTX mSha256Ctx;
    SHA512_CTX mSha512Ctx;
};

} // namespace eular

#endif // __CRYPTO_SHA_H__