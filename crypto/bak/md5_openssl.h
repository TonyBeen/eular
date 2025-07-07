/*************************************************************************
    > File Name: md5.h
    > Author: hsz
    > Brief: md5加密算法
    > Created Time: Wed 22 Dec 2021 10:07:20 AM CST
 ************************************************************************/

#ifndef __CRYPTO_MD5_H__
#define __CRYPTO_MD5_H__

#include "crypto.h"
#include <openssl/md5.h>

namespace eular {
class Md5 : public CryptoBase
{
public:
    Md5();
    virtual ~Md5();

    static const uint8_t MD5_BUF_SIZE = MD5_DIGEST_LENGTH;

    virtual int encode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen);
    virtual int decode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen);

protected:
    MD5_CTX mMd5;
};

} // namespace eular

#endif // __CRYPTO_MD5_H__