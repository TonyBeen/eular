/*************************************************************************
    > File Name: ras.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月11日 星期五 16时37分11秒
 ************************************************************************/

#ifndef __CRYPTO_RSA_H__
#define __CRYPTO_RSA_H__

#include <stdint.h>
#include <string>
#include <vector>
#include <memory>

namespace eular {
namespace crypto {
class RSAContex;
class Rsa
{
    Rsa(const Rsa &) = delete;
    Rsa &operator=(const Rsa &) = delete;
public:
    Rsa();
    Rsa(const std::string &publicKey, const std::string &privateKey);
    ~Rsa();

    enum HashMethod {
        MT_MD5,
        MT_SHA1,
        MT_SHA256,
        MT_SHA512,
    };

    static int32_t GenerateRSAKey(std::string &publicKey, std::string &privateKey, int32_t keyBits = 2048);
    static std::string Status2Msg(int32_t status);

    int32_t initRSAKey(const std::string &publicKey, const std::string &privateKey);

    /**
     * @brief 设置hash类型
     *
     * @param md HashMethod
     */
    void setHashMode(HashMethod md);

    /**
     * @brief 公钥加密
     *
     * @param data 
     * @param dataSize 
     * @param encryptedData 
     * @return int32_t 成功返回0, 失败返回错误码
     */
    int32_t publicEncrypt(const void *data, size_t dataSize, std::vector<uint8_t> &encryptedData);
    template<typename T>
    int32_t publicEncrypt(const std::basic_string<T> &data, std::vector<uint8_t> &encryptedData) {
        return publicEncrypt(data.data(), data.size() * sizeof(T), encryptedData);
    }

    /**
     * @brief 使用公钥校验签名
     *
     * @param signatureData 签名数据
     * @param signatureSize 签名数据长度
     * @param hashVec 源数据hash值
     * @return int32_t 成功返回0, 失败返回错误码
     */
    int32_t verifySignature(const void *signatureData, size_t signatureSize, const std::vector<uint8_t> &hashVec);
    int32_t verifySignature(const std::vector<uint8_t> &signatureData, const std::vector<uint8_t> &hashVec) {
        return verifySignature(signatureData.data(), signatureData.size(), hashVec);
    }

    /**
     * @brief 使用私钥进行签名
     *
     * @param hashVec 源数据hash值
     * @param signatureVec 签名数据
     * @return int32_t 成功返回0, 失败返回错误码
     */
    int32_t sign(const std::vector<uint8_t> &hashVec, std::vector<uint8_t> &signatureVec);

    /**
     * @brief 私钥解密
     *
     * @param data 
     * @param dataSize 
     * @param decryptedData 
     * @return int32_t 成功返回0, 失败返回错误码
     */
    int32_t privateDecrypt(const void *data, size_t dataSize, std::vector<uint8_t> &decryptedData);
    template<typename T>
    int32_t privateDecrypt(const std::vector<T> &data, std::vector<uint8_t> &decryptedData) {
        return privateDecrypt(data.data(), data.size() * sizeof(T), decryptedData);
    }

    /**
     * @brief 私钥解密
     *
     * @param data 
     * @param dataSize 
     * @param decryptedData 
     * @return int32_t 成功返回0, 失败返回错误码
     */
    int32_t privateDecrypt(const void *data, size_t dataSize, std::string &decryptedData);
    template<typename T>
    int32_t privateDecrypt(const std::basic_string<T> &data, std::string &decryptedData) {
        return privateDecrypt(data.data(), data.size() * sizeof(T), decryptedData);
    }
    template<typename T>
    int32_t privateDecrypt(const std::vector<T> &data, std::string &decryptedData) {
        return privateDecrypt(data.data(), data.size() * sizeof(T), decryptedData);
    }

private:
    int32_t rsaPaddingSize(const void *key, int32_t &keySize) const;

private:
    std::unique_ptr<RSAContex>  m_context;
};

} // namespace crypto
} // namespace eular

#endif // __CRYPTO_RSA_H__
