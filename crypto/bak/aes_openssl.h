/*************************************************************************
    > File Name: aes.h
    > Author: hsz
    > Brief:
    > Created Time: Wed 22 Dec 2021 10:09:08 AM CST
 ************************************************************************/

#ifndef __CRYPTO_AES_H__
#define __CRYPTO_AES_H__

#include "crypto.h"
#include <utils/buffer.h>
#include <openssl/aes.h>
#include <memory>

#define MAX_USER_KEY_SIZE 32
#define CBC_VECTOR_SIZE 16

namespace eular {
class Aes : public CryptoBase
{
public:
    enum KeyType {
        AES128 = 16,
        AES256 = 32,
    };

    enum EncodeType {
        AESECB = 0,
        AESCBC = 1,
    };

    Aes();
    Aes(const uint8_t *userKey, uint32_t userKeyLen, Aes::KeyType userKeytype, Aes::EncodeType encodeType);
    virtual ~Aes();

    bool reinit(const uint8_t *userKey, uint32_t userKeyLen, Aes::KeyType userKeytype, Aes::EncodeType encodeType);

    uint8_t getKeyType() const { return mUserKeyType; }
    void setKeyType(uint8_t keyType) { mUserKeyType = keyType; }

    uint8_t getEncodeType() const { return mEncodeType; }
    void setEncodeType(uint8_t encodeType) { mEncodeType = encodeType; }

    const uint8_t *getKey() const { return mUserKey; }
    void setKey(const uint8_t *key, uint32_t len);

    int32_t getEncodeLength(uint32_t contentLength) const;
    int32_t getDecodeLength(uint32_t contentLength) const;

    /**
     * @brief 对src的内容进行加密, 输出到out
     *
     * @param out 加密数据缓存, 请使用getEncodeLength提前获取加密后数据长度以保证函数不会越界
     * @param src 未加密数据缓存
     * @param srcLen 未加密数据长度
     * @return int32_t 返回加密后的实际长度, 失败返回负值
     */
    int32_t encode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen) override;

    /**
     * @brief 对src的内容进行解密
     *
     * @param out 解密数据缓存, 使用getDecodeLength获取解密后数据长度用以分配空间, 此长度大于等于实际解密后的数据长度
     * @param src 解密的数据缓存
     * @param srcLen 要解密的数据长度
     * @return int32_t 返回解密后的实际长度, 失败返回负值
     */
    int32_t decode(uint8_t *out, const uint8_t *src, const uint32_t &srcLen) override;

protected:
    std::shared_ptr<ByteBuffer> _pkcs7Padding(const uint8_t *in, uint32_t inLen);

protected:
    AES_KEY mAesKey;
    uint8_t mUserKey[MAX_USER_KEY_SIZE];
    uint8_t mUserKeyType;
    uint8_t mEncodeType;
    uint8_t vecForCBC[CBC_VECTOR_SIZE];
};

} // namespace eular

#endif // __CRYPTO_AES_H__