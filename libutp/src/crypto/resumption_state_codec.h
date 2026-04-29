/*************************************************************************
    > File Name: resumption_state_codec.h
    > Author: eular
    > Brief:
    > Created Time: Tue 24 Mar 2026 10:00:00 AM CST
 ************************************************************************/

#ifndef __UTP_CRYPTO_RESUMPTION_STATE_CODEC_H__
#define __UTP_CRYPTO_RESUMPTION_STATE_CODEC_H__

#include <cstdint>
#include <array>
#include <vector>

namespace eular {
namespace utp {

class ResumptionStateCodec
{
public:
    static constexpr size_t KEY_SIZE = 32;
    using Key = std::array<uint8_t, KEY_SIZE>;

    /**
     * @brief Seal 将明文加密封装成密文, 使用 AES-GCM 进行加密, 包含认证标签
     *
     * @param key 加密密钥, 长度必须为 KEY_SIZE
     * @param plaintext 待加密的明文数据
     * @param sealed 输出参数, 存储加密后的密文数据
     * @return true 加密成功
     * @return false 加密失败, 可能原因包括密钥长度不正确、加密过程错误等
     */
    static bool Seal(const Key &key, const std::vector<uint8_t> &plaintext, std::vector<uint8_t> &sealed);

    /**
     * @brief Open 将密文解密还原成明文, 验证密文的完整性和真实性, 使用 AES-GCM 进行解密
     *
     * @param key 解密密钥, 长度必须为 KEY_SIZE, 必须与 Seal 使用的密钥相同
     * @param sealed 待解密的密文数据, 包含认证标签
     * @param plaintext 输出参数, 存储解密后的明文数据
     * @return true 解密成功, plaintext 中包含正确的明文数据
     * @return false 解密失败, 可能原因包括密钥长度不正确、密文被篡改、认证失败等, plaintext 不应使用
     */
    static bool Open(const Key &key, const std::vector<uint8_t> &sealed, std::vector<uint8_t> &plaintext);
};

} // namespace utp
} // namespace eular

#endif // __UTP_CRYPTO_RESUMPTION_STATE_CODEC_H__