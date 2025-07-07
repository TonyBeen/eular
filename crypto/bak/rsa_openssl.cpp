/*************************************************************************
    > File Name: rsa.cpp
    > Author: hsz
    > Brief: 非对称加密，非对称加密传输对称加密的秘钥，对称加密加密数据
    > Created Time: Wed 22 Dec 2021 10:10:05 AM CST
 ************************************************************************/

/**
 * 公钥和私钥是一对，用公钥对数据加密，只能用对应的私钥解密。用私钥对数据加密，只能用对应的公钥进行解密。
 * 由于非对称加密的计算复杂，计算时间长，而对称加密加密所消耗的时间少，但由于秘钥在传递过程中容易泄露，
 * 所以采用非对称加密对'对称加密'的秘钥进行加密后传递
 * 但是，公钥所有人都可以获得，那通过秘钥加密的数据岂不是也可以解出来！
 */

#include "rsa_openssl.h"
#include <utils/errors.h>
#include <utils/utils.h>
#include <log/log.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#define LOG_TAG "RSA"

namespace eular {
Rsa::Rsa(const String8 &pubkeyFile, const String8 &prikeyFile) :
    mPublicKey(nullptr),
    mPrivatKey(nullptr),
    mPubKeyPath(pubkeyFile),
    mPriKeyPath(prikeyFile)
{
    reinit();
}

Rsa::~Rsa()
{
    destroy();
}

int32_t Rsa::GenerateKey(const String8 &pubkeyFile, const String8 &prikeyFile, uint32_t len)
{
    RSA *rsa = nullptr;
    FILE *fp = nullptr;
    int32_t ret = OK;

    LOGD("openssl version: %s", OPENSSL_VERSION_TEXT);
    do {
        rsa = RSA_generate_key(len, RSA_F4, NULL, NULL);
        if (!rsa) {
            ret = (int32_t)ERR_get_error();
            LOGE("RSA_generate_key failed. [%d, %s]", ret, ERR_error_string(ret, NULL));
            ret = UNKNOWN_ERROR;
            break;
        }

        fp = fopen(pubkeyFile.c_str(), "w+");
        if (!fp) {
            LOGE("fopen error. [%d, %s]", errno, strerror(errno));
            ret = UNKNOWN_ERROR;
            break;
        }
        PEM_write_RSAPublicKey(fp, rsa);
        fclose(fp);
        fp = nullptr;

        fp = fopen(prikeyFile.c_str(), "w+");
        if (!fp) {
            LOGE("fopen error. [%d, %s]", errno, strerror(errno));
            ret = UNKNOWN_ERROR;
            break;
        }
        PEM_write_RSAPrivateKey(fp, rsa, NULL, NULL, 0, NULL, NULL);
        fclose(fp);
        fp = nullptr;
    } while(0);
    
    if (rsa) {
        RSA_free(rsa);
    }
    if (fp) {
        fclose(fp);
    }

    return ret;
}

int32_t Rsa::publicEncode(uint8_t *out, const uint8_t *src, uint32_t srcLen)
{
    if (!out || !src || !srcLen) {
        return Status::INVALID_PARAM;
    }
    int32_t canEncodeLen = getPubRsaSize();
    int32_t realLen = 0;    // 实际一次可编码最大字节数
    switch (defaultPadding) {
    case RSA_PKCS1_PADDING:
        realLen = canEncodeLen - 11;
        break;
    case RSA_NO_PADDING:
        realLen = canEncodeLen;
        break;
    case RSA_X931_PADDING:
        realLen = canEncodeLen - 2;
        break;
    default:
        LOGE("unknow padding type: %d", defaultPadding);
        return Status::UNKNOWN_ERROR;
    }
    int32_t totalEncodeSize = 0;
    int32_t encodeLen = 0;  // 一次编码返回值
    const uint8_t *from = src;
    uint8_t *to = out;

    while (srcLen > (uint32_t)realLen) {
        encodeLen = RSA_public_encrypt(realLen, from, to, mPublicKey, defaultPadding);
        if (encodeLen < 0) {
            int32_t ret = (int32_t)ERR_get_error();
            LOGE("RSA_public_encrypt failed. [%d, %s]", ret, ERR_error_string(ret, NULL));
            return UNKNOWN_ERROR;
        }
        totalEncodeSize += encodeLen;
        from += realLen;
        to += encodeLen;
    
        srcLen -= realLen;
        LOGI("encode size = %d, srcLen = %d", encodeLen, srcLen);
    }

    // 不足realLen字节
    encodeLen = RSA_public_encrypt(srcLen, from, to, mPublicKey, defaultPadding);
    if (encodeLen < 0) {
        int32_t ret = (int32_t)ERR_get_error();
        LOGE("RSA_public_encrypt failed. [%d, %s]", ret, ERR_error_string(ret, NULL));
        return UNKNOWN_ERROR;
    }
    totalEncodeSize += encodeLen;

    return totalEncodeSize;
}

Rsa::BufferPtr Rsa::publicEncode(const uint8_t *from, uint32_t fromLen)
{
    if (!from || !fromLen) {
        return nullptr;
    }
    Rsa::BufferPtr ptr(new ByteBuffer);
    ptr->resize(getEncodeSpaceByDataLen(fromLen, false) + 1);
    int32_t encodeSize = publicEncode(ptr->data(), from, fromLen);
    ptr->resize(encodeSize);
    return encodeSize > 0 ? ptr : nullptr;
}

int32_t Rsa::publicEncode(ByteBuffer &out, const uint8_t *src, uint32_t srcLen)
{
    if (!src || !srcLen) {
        return INVALID_PARAM;
    }

    out.resize(getEncodeSpaceByDataLen(srcLen, false) + 1);
    uint8_t *outputBuf = out.data();
    int32_t encodeSize = publicEncode(outputBuf, src, srcLen);
    out.resize(encodeSize);
    return encodeSize;
}

int32_t Rsa::publicDecode(uint8_t *out, const uint8_t *src, uint32_t srcLen)
{
    if (!out || !src || !srcLen) {
        return INVALID_PARAM;
    }

    int32_t canDecodeLen = getPubRsaSize();
    const uint8_t *from = src;
    uint8_t *to = out;
    int32_t totalDecodeSize = 0;
    int32_t decodeLen = 0;

    while (srcLen > (uint32_t)canDecodeLen) {
        decodeLen = RSA_public_decrypt(canDecodeLen, from, to, mPublicKey, defaultPadding);
        if (decodeLen < 0) {
            int32_t ret = (int32_t)ERR_get_error();
            LOGE("RSA_public_decrypt failed. [%d, %s]", ret, ERR_error_string(ret, NULL));
            return UNKNOWN_ERROR;
        }

        to += decodeLen;
        from += canDecodeLen;
        totalDecodeSize += decodeLen;
        srcLen -= canDecodeLen;
    }

    decodeLen = RSA_public_decrypt(srcLen, from, to, mPublicKey, defaultPadding);
    if (decodeLen < 0) {
        int32_t ret = (int32_t)ERR_get_error();
        LOGE("RSA_public_decrypt failed. [%d, %s]", ret, ERR_error_string(ret, NULL));
        return UNKNOWN_ERROR;
    }
    totalDecodeSize += decodeLen;

    return totalDecodeSize;
}

Rsa::BufferPtr Rsa::publicDecode(const uint8_t *from, uint32_t fromLen)
{
    if (!from || !fromLen) {
        return nullptr;
    }

    BufferPtr ptr(new ByteBuffer());
    if (ptr != nullptr) {
        ptr->resize(getDecodeSpaceByDataLen(fromLen, false) + 1);
        int32_t decodeSize = publicDecode(ptr->data(), from, fromLen);
        ptr->resize(decodeSize);
    }

    return ptr;
}

int32_t Rsa::publicDecode(ByteBuffer &out, const uint8_t *src, uint32_t srcLen)
{
    if (!src || !srcLen) {
        return INVALID_PARAM;
    }

    out.resize(getDecodeSpaceByDataLen(srcLen, false) + 1);
    uint8_t *outputBuf = out.data();
    int32_t decodeSize = publicDecode(outputBuf, src, srcLen);
    out.resize(decodeSize);

    return decodeSize;
}


int32_t Rsa::privateEncode(uint8_t *out, const uint8_t *src, uint32_t srcLen)
{
    if (!out || !src || !srcLen) {
        return INVALID_PARAM;
    }

    int32_t canEncodeSize = getPriRsaSize();
    int32_t metaSize = 0;   // 实际单元加密数据块大小，受padding影响
    switch (defaultPadding) {
    case RSA_PKCS1_PADDING:
        metaSize = canEncodeSize - 11;
        break;
    case RSA_NO_PADDING:
        metaSize = canEncodeSize;
        break;
    case RSA_X931_PADDING:
        metaSize = canEncodeSize - 2;
        break;
    default:
        LOGE("unknow padding type: %d", defaultPadding);
        return UNKNOWN_ERROR;
    }

    const uint8_t *from = src;
    uint8_t *to = out;
    int32_t totalEncodeSize = 0;
    int32_t encodeSize = 0;

    while (srcLen > (uint32_t)metaSize) {
        encodeSize = RSA_private_encrypt(metaSize, from, to, mPrivatKey, defaultPadding);
        if (encodeSize < 0) {
            int32_t ret = (int32_t)ERR_get_error();
            LOGE("RSA_private_decrypt failed. [%d, %s]", ret, ERR_error_string(ret, NULL));
            return UNKNOWN_ERROR;
        }

        totalEncodeSize += encodeSize;
        from += metaSize;
        to += encodeSize;
        srcLen -= metaSize;
    }
    
    encodeSize = RSA_private_encrypt(metaSize, from, to, mPrivatKey, defaultPadding);
    if (encodeSize < 0) {
        int32_t ret = (int32_t)ERR_get_error();
        LOGE("RSA_private_decrypt failed. [%d, %s]", ret, ERR_error_string(ret, NULL));
        return UNKNOWN_ERROR;
    }

    totalEncodeSize += encodeSize;
    return totalEncodeSize;
}

Rsa::BufferPtr Rsa::privateEncode(const uint8_t *from, uint32_t fromLen)
{
    return nullptr;
}

int32_t Rsa::privateEncode(ByteBuffer &out, const uint8_t *src, uint32_t srcLen)
{
    return 0;
}

int32_t Rsa::privateDecode(uint8_t *out, const uint8_t *src, uint32_t srcLen)
{
    if (!out || !src || !srcLen) {
        return INVALID_PARAM;
    }
    int32_t canDecodeLen = getPriRsaSize();

    const uint8_t *from = src;
    uint8_t *to = out;
    int32_t totalDecodeSize = 0;
    int32_t decodeLen = 0;
    while (srcLen > (uint32_t)canDecodeLen) {
        decodeLen = RSA_private_decrypt(canDecodeLen, from, to, mPrivatKey, defaultPadding);
        if (decodeLen < 0) {
            int32_t ret = (int32_t)ERR_get_error();
            LOGE("RSA_private_decrypt failed. [%d, %s]", ret, ERR_error_string(ret, NULL));
            return UNKNOWN_ERROR;
        }
        from += canDecodeLen;
        to += decodeLen;
        totalDecodeSize += decodeLen;
        srcLen -= canDecodeLen;
        LOGD("decode size: %d; Number of remaining uncoded bytes: %d", totalDecodeSize, srcLen);
    }

    decodeLen = RSA_private_decrypt(srcLen, from, to, mPrivatKey, defaultPadding);
    if (decodeLen < 0) {
        int32_t ret = (int32_t)ERR_get_error();
        LOGE("RSA_private_decrypt failed. [%d, %s]", ret, ERR_error_string(ret, NULL));
        return UNKNOWN_ERROR;
    }
    totalDecodeSize += decodeLen;

    return totalDecodeSize;
}

Rsa::BufferPtr Rsa::privateDecode(const uint8_t *from, uint32_t fromLen)
{
    return nullptr;
}

int32_t Rsa::privateDecode(ByteBuffer &out, const uint8_t *src, uint32_t srcLen)
{
    return 0;
}

bool Rsa::reinit()
{
    bool ret = true;
    FILE *fp = nullptr;
    int32_t err = 0;

    mPubKeyStr.clear();
    mPriKeyStr.clear();
    do {
        fp = fopen(mPubKeyPath.c_str(), "r");
        if (!fp) {
            LOGE("fopen error. [%d, %s]", errno, strerror(errno));
            ret = false;
            break;
        }

        mPublicKey = PEM_read_RSAPublicKey(fp, NULL, NULL, NULL);
        if (!mPublicKey) {
            err = (int32_t)ERR_get_error();
            LOGE("RSA_generate_key failed. [%d, %s]", err, ERR_error_string(err, NULL));
            ret = false;
            break;
        }
        int32_t read = 0;
        char buf[256] = {0};
        fseek(fp, 0, SEEK_SET);
        do {
            read = fread(buf, 1, sizeof(buf), fp);
            if (read > 0) {
                mPubKeyStr.append(buf);
            } else if (read < 0) {
                LOGE("fread error. [%d, %s]", errno, strerror(errno));
            }
            memset(buf, 0, sizeof(buf));
        } while (read > 0);
        fclose(fp);

        fp = fopen(mPriKeyPath.c_str(), "r");
        if (!fp) {
            LOGE("fopen error. [%d, %s]", errno, strerror(errno));
            ret = false;
            break;
        }

        mPrivatKey = PEM_read_RSAPrivateKey(fp, NULL, NULL, NULL);
        if (!mPrivatKey) {
            err = (int32_t)ERR_get_error();
            LOGE("RSA_generate_key failed. %d %s", err, ERR_error_string(err, NULL));
            ret = false;
            break;
        }
        fseek(fp, 0, SEEK_SET);
        do {
            read = fread(buf, 1, sizeof(buf), fp);
            if (read > 0) {
                mPriKeyStr.append(buf);
            } else if (read < 0) {
                LOGE("fread error. [%d, %s]", errno, strerror(errno));
            }
            memset(buf, 0, sizeof(buf));
        } while (read > 0);
    } while(0);

    if (fp) {
        fclose(fp);
    }
    return ret;
}

void Rsa::destroy()
{
    if (mPublicKey) {
        RSA_free(mPublicKey);
        mPublicKey = nullptr;
    }
    if (mPrivatKey) {
        RSA_free(mPrivatKey);
        mPrivatKey = nullptr;
    }
}

uint32_t Rsa::getDecodeSpaceByDataLen(uint32_t len, bool priKeyDecode)
{
    int32_t metaSize = 0;   // 解密时的单位数据块大小
    if (priKeyDecode) {
        metaSize = getPriRsaSize();
    } else {
        metaSize = getPubRsaSize();
    }
    int32_t realLen = 0;    // 一个单位数据块实际解出来的数据长度
    switch (defaultPadding) {
    case RSA_PKCS1_PADDING:
        realLen = metaSize - 11;
        break;
    case RSA_NO_PADDING:
        realLen = metaSize;
        break;
    case RSA_X931_PADDING:
        realLen = metaSize - 2;
        break;
    default:
        LOGE("unknow padding type: %d", defaultPadding);
        return 0;
    }
    int32_t index = len / metaSize; // 存在多少个单位数据
    if (len % metaSize) {
        index++;
    }
    
    return realLen * index;
}

uint32_t Rsa::getEncodeSpaceByDataLen(uint32_t len, bool priKeyEncode)
{
    int32_t metaSize = 0;           // 单位数据块加密后产生的数据块大小
    if (priKeyEncode) {
        metaSize = getPubRsaSize();
    } else {
        metaSize = getPriRsaSize();
    }

    int32_t realMetaSize = 0;       // 一个单位要加密的数据块大小，受padding影响
    switch (defaultPadding) {
    case RSA_PKCS1_PADDING:
        realMetaSize = metaSize - 11;
        break;
    case RSA_NO_PADDING:
        realMetaSize = metaSize;
        break;
    case RSA_X931_PADDING:
        realMetaSize = metaSize - 2;
        break;
    default:
        LOGE("unknow padding type: %d", defaultPadding);
        return 0;
    }

    // 一个realMetaSize大小的数据块加密后产生metaSize大小的数据块
    int32_t index = len / realMetaSize;
    if (len % realMetaSize) {
        index++;
    }

    return metaSize * index;
}

} // namespace eular
