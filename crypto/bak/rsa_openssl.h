/*************************************************************************
    > File Name: rsa.h
    > Author: hsz
    > Brief:
    > Created Time: Wed 22 Dec 2021 10:10:02 AM CST
 ************************************************************************/

#ifndef __CRYPTO_RSA_H__
#define __CRYPTO_RSA_H__

#include "crypto.h"
#include <utils/buffer.h>
#include <utils/string8.h>
#include <openssl/rsa.h>
#include <memory>

namespace eular {
class Rsa
{
public:
    Rsa(const String8 &pubkeyFile, const String8 &prikeyFile);
    virtual ~Rsa();

    using BufferPtr = std::shared_ptr<ByteBuffer>;

    static int32_t GenerateKey(const String8 &pubkeyFile, const String8 &prikeyFile, uint32_t len = 1024);

    /**
     * @brief 使用公钥加密
     * @param out 加密输出位置
     * @param src 要加密的数据
     * @param srcLen src数据的长度
     * @return 成功返回加密的长度，失败返回负值
     */
    int32_t publicEncode(uint8_t *out, const uint8_t *src, uint32_t srcLen);
    int32_t publicEncode(ByteBuffer &out, const uint8_t *src, uint32_t srcLen);
    BufferPtr publicEncode(const uint8_t *from, uint32_t fromLen);

    /**
     * @brief 使用公钥加密
     * @param out 解密输出位置
     * @param src 要解密的数据
     * @param srcLen src数据的长度
     * @return 成功返回解密的长度，失败返回负值
     */
    int32_t publicDecode(uint8_t *out, const uint8_t *src, uint32_t srcLen);
    int32_t publicDecode(ByteBuffer &out, const uint8_t *src, uint32_t srcLen);
    BufferPtr publicDecode(const uint8_t *from, uint32_t fromLen);

    /**
     * @brief 使用私钥加密
     * @param out 加密输出位置
     * @param src 要加密的数据
     * @param srcLen src数据的长度
     * @return 成功返回加密的长度，失败返回负值
     */
    int32_t privateEncode(uint8_t *out, const uint8_t *src, uint32_t srcLen);
    int32_t privateEncode(ByteBuffer &out, const uint8_t *src, uint32_t srcLen);
    BufferPtr privateEncode(const uint8_t *from, uint32_t fromLen);

    /**
     * @brief 使用私钥解密
     * @param out 解密输出位置
     * @param src 要解密的数据
     * @param srcLen src数据的长度
     * @return 成功返回解密的长度，失败返回负值
     */
    int32_t privateDecode(uint8_t *out, const uint8_t *src, uint32_t srcLen);
    int32_t privateDecode(ByteBuffer &out, const uint8_t *src, uint32_t srcLen);
    BufferPtr privateDecode(const uint8_t *from, uint32_t fromLen);

    // 根据秘钥或公钥获得单位加密/解密字节数，受padding影响
    int32_t getPubRsaSize() const { return RSA_size(mPublicKey); }
    int32_t getPriRsaSize() const { return RSA_size(mPrivatKey); }

    uint32_t getDecodeSpaceByDataLen(uint32_t len, bool priKeyDecode = true);
    uint32_t getEncodeSpaceByDataLen(uint32_t len, bool priKeyEncode = true);

private:
    bool reinit();
    void destroy();

protected:
    RSA *mPublicKey;
    RSA *mPrivatKey;

    String8 mPubKeyPath;
    String8 mPriKeyPath;
    String8 mPubKeyStr;
    String8 mPriKeyStr;
    static const int32_t defaultPadding = RSA_PKCS1_PADDING;
};

} // namespace eular

#endif // __CRYPTO_RSA_H__