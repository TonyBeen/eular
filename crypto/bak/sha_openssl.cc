/*************************************************************************
    > File Name: sha.cpp
    > Author: hsz
    > Brief:
    > Created Time: Wed 22 Dec 2021 10:08:26 AM CST
 ************************************************************************/

#include "sha_openssl.h"

namespace eular {

Sha::Sha(SHA_TYPE whichType)
{
    reinit(whichType);
}

Sha::~Sha()
{

}

bool Sha::reinit(SHA_TYPE whichType)
{
    switch (whichType) {
    case SHA_TYPE::SHA256:
        SHA256_Init(&mSha256Ctx);
        break;
    case SHA_TYPE::SHA512:
        SHA512_Init(&mSha512Ctx);
        break;
    default:
        throw Exception(String8::Format("Invalid SHA_TYPE type: %d", (int)whichType));
        break;
    }

    mShaType = whichType;
    return true;
}

int Sha::encode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen)
{
    int ret = NO_INIT;
    switch (mShaType) {
    case SHA_TYPE::SHA256:
        ret = EncodeSha256(out, src, srcLen);
        break;
    case SHA_TYPE::SHA512:
        ret = EncodeSha512(out, src, srcLen);
        break;
    default:
        break;
    }
    return ret;
}

int Sha::decode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen)
{
    return -1;
}

int Sha::EncodeSha256(uint8_t *out, const uint8_t *src, uint32_t len)
{
    SHA256_Update(&mSha256Ctx, src, len);
    SHA256_Final(out, &mSha256Ctx);

    return 0;
}

int Sha::EncodeSha512(uint8_t *out, const uint8_t *src, uint32_t len)
{
    SHA512_Update(&mSha512Ctx, src, len);
    SHA512_Final(out, &mSha512Ctx);

    return 0;
}

} // namespace eular
