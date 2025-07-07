/*************************************************************************
    > File Name: aes.cpp
    > Author: hsz
    > Brief: 对称加密，速度快于非对称加密
    > Created Time: Wed 22 Dec 2021 10:09:11 AM CST
 ************************************************************************/

#include "aes_openssl.h"
#include <utils/errors.h>
#include <utils/exception.h>
#include <utils/string8.h>
#include <log/log.h>

#define LOG_TAG "AES"

namespace eular {
Aes::Aes() :
    mUserKeyType(KeyType::AES128),
    mEncodeType(EncodeType::AESCBC)
{
    memset(vecForCBC, 0, CBC_VECTOR_SIZE);
}

Aes::Aes(const uint8_t *userKey, uint32_t userKeyLen, Aes::KeyType userKeytype, Aes::EncodeType encodeType) :
    mUserKeyType(KeyType::AES128),
    mEncodeType(EncodeType::AESCBC)
{
    memset(vecForCBC, 0, CBC_VECTOR_SIZE);
    reinit(userKey, userKeyLen, userKeytype, encodeType);
}

Aes::~Aes()
{

}

bool Aes::reinit(const uint8_t *userKey, uint32_t userKeyLen, Aes::KeyType userKeytype, Aes::EncodeType encodeType)
{
    if (userKey == nullptr) {
        return false;
    }

    switch (userKeytype) {
    case Aes::KeyType::AES128:
    case Aes::KeyType::AES256:
        break;
    default:
        throw(Exception(String8::format("Invalid AES Key Type: %d", userKeytype)));
    }

    switch (encodeType) {
    case EncodeType::AESCBC:
    case EncodeType::AESECB:
        break;
    default:
        throw(Exception(String8::format("Invalid AES Encode Type: %d", encodeType)));
    }

    if (userKeyLen == 0 || userKeyLen > userKeytype) {
        return false;
    }

    memset(mUserKey, 0, MAX_USER_KEY_SIZE);
    memcpy(mUserKey, userKey, userKeyLen);
    mUserKeyType = userKeytype;
    mEncodeType = encodeType;

    return true;
}

void Aes::setKey(const uint8_t *key, uint32_t len)
{
    if (key && (0 < len && len <= MAX_USER_KEY_SIZE)) {
        memset(mUserKey, 0, MAX_USER_KEY_SIZE);
        memcpy(mUserKey, key, len);
    }
}

int32_t Aes::getEncodeLength(uint32_t contentLength) const
{
    if (contentLength == 0) {
        return 0;
    }

    return ((contentLength + AES_BLOCK_SIZE) / AES_BLOCK_SIZE) * AES_BLOCK_SIZE;
}

int32_t Aes::getDecodeLength(uint32_t contentLength) const
{
    return contentLength;
}

int32_t Aes::encode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen)
{
    if (out == nullptr || src == nullptr || srcLen == 0) {
        return Status::INVALID_PARAM;
    }

    std::shared_ptr<ByteBuffer> ptr = _pkcs7Padding(src, srcLen);
    if (ptr == nullptr) {
        return Status::NO_MEMORY;
    }

    AES_set_encrypt_key(mUserKey, mUserKeyType * 8, &mAesKey);
    switch (mEncodeType) {
    case EncodeType::AESECB:
        AES_ecb_encrypt(ptr->const_data(), out, &mAesKey, AES_ENCRYPT);
        break;
    case EncodeType::AESCBC:
        memset(vecForCBC, 0, AES_BLOCK_SIZE);   // 加密和解密时初始vecForCBC内容须一致，一般设置为全0
        AES_cbc_encrypt(ptr->const_data(), out, ptr->size(), &mAesKey, vecForCBC, AES_ENCRYPT);
        break;
    default:
        return Status::INVALID_PARAM;
    }

    return ptr->size();
}

int32_t Aes::decode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen)
{
    AES_set_decrypt_key(mUserKey, mUserKeyType * 8, &mAesKey);
    if (mEncodeType == AESECB) {
        AES_ecb_encrypt(src, out, &mAesKey, AES_DECRYPT);
    } else if (mEncodeType == AESCBC) {
        memset(vecForCBC, 0, AES_BLOCK_SIZE);
        AES_cbc_encrypt(src, out, srcLen, &mAesKey, vecForCBC, AES_DECRYPT);
    }

    uint8_t paddingSize = 0;
    if (out[srcLen - 1] == 0x1) {    // 填充了一个
        paddingSize = 0x1;
    } else if (out[srcLen - 1] == out[srcLen - 2]) {
        paddingSize = out[srcLen - 1];
    }

    out[srcLen - paddingSize] = '\0';
    return srcLen - paddingSize;
}

std::shared_ptr<ByteBuffer> Aes::_pkcs7Padding(const uint8_t *in, uint32_t inLen)
{
    std::shared_ptr<ByteBuffer> ptr(new (std::nothrow)ByteBuffer(in, inLen));
    if (ptr == nullptr) {
        return nullptr;
    }

    uint8_t remainderSize = inLen % AES_BLOCK_SIZE;
    if (remainderSize == 0) {
        return ptr;
    }

    uint8_t buf[AES_BLOCK_SIZE] = {0};
    uint8_t paddingSize = AES_BLOCK_SIZE - remainderSize;

    memset(buf, paddingSize, paddingSize);
    ptr->append(buf, paddingSize);

    return ptr;
}

} // namespace eular
