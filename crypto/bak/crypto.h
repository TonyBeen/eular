/*************************************************************************
    > File Name: crypto.h
    > Author: hsz
    > Brief: 编解码基类
    > Created Time: Tue 21 Dec 2021 02:35:32 PM CST
 ************************************************************************/

#ifndef __CRYPTO_H__
#define __CRYPTO_H__

#include <stdint.h>

namespace eular
{

class CryptoBase
{
public:
    CryptoBase() {}
    virtual ~CryptoBase() {}

    virtual int encode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen) = 0;
    virtual int decode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen) = 0;
};

} // namespace eular

#endif