/*************************************************************************
    > File Name: aes_gcm_context.h
    > Author: eular
    > Brief:
    > Created Time: Tue 06 Jan 2026 04:49:17 PM CST
 ************************************************************************/

#ifndef __UTP_CRYPTO_AES_GCM_CONTEXT_H__
#define __UTP_CRYPTO_AES_GCM_CONTEXT_H__

#include <array>
#include <cstddef>
#include <memory>

#include <openssl/evp.h>
#include <openssl/err.h>

namespace eular {
namespace utp {
struct PacketOut;
struct PacketIn;

class AesGcmContext
{
public:
    enum {
        GCM_TAG_SIZE = 16,
        GCM_NONCE_SIZE = 12,
    };

    using AesKey128 = std::array<uint8_t, 16>; // AES-128 key size
    using AesKey256 = std::array<uint8_t, 32>; // AES-256 key size
    using Nonce     = std::array<uint8_t, GCM_NONCE_SIZE>;
    struct PlainSegment {
        const uint8_t *data;
        size_t len;

        PlainSegment()
            : data(nullptr), len(0)
        {
        }

        PlainSegment(const uint8_t *segmentData, size_t segmentLen)
            : data(segmentData), len(segmentLen)
        {
        }
    };
    using Ptr       = std::unique_ptr<AesGcmContext>;
    using SP        = std::shared_ptr<AesGcmContext>;

    AesGcmContext() = default;
    ~AesGcmContext();

    // 禁止拷贝
    AesGcmContext(const AesGcmContext&) = delete;
    AesGcmContext& operator=(const AesGcmContext&) = delete;

    // 允许移动
    AesGcmContext(AesGcmContext&& other) noexcept;
    AesGcmContext& operator=(AesGcmContext&& other) noexcept;

    /**
     * @brief 初始化 AES-128-GCM 上下文
     *
     * @param key AES-128 密钥 (16 字节)
     * @param noncePerfix Nonce 前缀, 内部会转为大端存储 (4 字节)
     */
    bool init(const AesKey128& key, uint32_t noncePerfix);

    /**
     * @brief 初始化 AES-256-GCM 上下文
     *
     * @param key AES-256 密钥 (32 字节)
     * @param noncePerfix Nonce 前缀, 内部会转为大端存储 (4 字节)
     */
    bool init(const AesKey256& key, uint32_t noncePerfix);

    int32_t encrypt(PacketOut *packet);
    int32_t decrypt(PacketIn *packet);

    /**
     * @brief 加密
     *
     * @param plaintext 明文
     * @param plaintext_len 明文长度
     * @param aad packet头部24字节(排除校验和)
     * @param aad_len 24字节
     * @param counter packet 序号
     * @param ciphertext 加密后输出缓冲区
     * @param[in,out] ciphertext_len 输入：缓冲区大小; 输出: 加密后长度(包含 tag 长度 16 字节)
     * @return int32_t 加密数据长度
     */
    int32_t encrypt(const uint8_t* plaintext, size_t plaintext_len,
                    const uint8_t* aad, size_t aad_len,
                    uint64_t counter,
                    uint8_t* ciphertext, size_t* ciphertext_len);

    /**
     * @brief 对多段明文执行一次 AES-GCM 加密，密文连续输出
     */
    int32_t encryptScatter(const PlainSegment *segments,
                           size_t segmentCount,
                           const uint8_t *aad,
                           size_t aad_len,
                           uint64_t counter,
                           uint8_t *ciphertext,
                           size_t *ciphertext_len);

    /**
     * @brief 解密
     *
     * @param ciphertext 加密数据
     * @param ciphertext_len 加密数据长度(包含 tag 长度 16 字节)
     * @param aad packet头部24字节(排除校验和)
     * @param aad_len 24字节
     * @param counter packet 序号
     * @param plaintext 明文输出缓冲区
     * @param[in,out] plaintext_len 输入：缓冲区大小; 输出: 明文长度
     * @return int32_t 明文数据长度
     */
    int32_t decrypt(const uint8_t* ciphertext, size_t ciphertext_len,
                    const uint8_t* aad, size_t aad_len,
                    uint64_t counter,
                    uint8_t* plaintext, size_t* plaintext_len);

private:
    Nonce buildNonce(uint64_t counter) const;
    void  cleanup();

private:
    const EVP_CIPHER*      m_cipher{nullptr};
    std::array<uint8_t, 32> m_key{};
    size_t                  m_keySize{0};
    std::array<uint8_t, 4> m_noncePerfix; // 96-bit nonce for AES-GCM
};

} // namespace utp
} // namespace eular

#endif // __UTP_CRYPTO_AES_GCM_CONTEXT_H__
