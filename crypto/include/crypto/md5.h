/*************************************************************************
    > File Name: md5.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月07日 星期一 11时29分15秒
 ************************************************************************/

#ifndef __CRYPTO_MD5_H__
#define __CRYPTO_MD5_H__

#include <stdint.h>
#include <string>
#include <vector>
#include <memory>

#ifndef MD5_DIGEST_LENGTH
#define MD5_DIGEST_LENGTH   16
#endif

namespace eular {
namespace crypto {
class MD5Context;
class MD5
{
public:
    MD5() = default;
    ~MD5() = default;

    /**
     * @brief 计算数据的MD5哈希值
     *
     * @param data 数据
     * @param bytes 数据字节数
     * @return std::string 成功返回MD5哈希值，失败返回空字符串
     */
    static std::string Hash(const void *data, int32_t bytes);
    template <typename T>
    static std::string Hash(const std::basic_string<T> &data) {
        if (data.empty()) {
            return std::string();
        }
        return Hash(data.data(), static_cast<int32_t>(data.size() * sizeof(T)));
    }
    template <typename T>
    static std::string Hash(const std::vector<T> &data) {
        if (data.empty()) {
            return std::string();
        }
        return Hash(data.data(), static_cast<int32_t>(data.size() * sizeof(T)));
    }

    /**
     * @brief 初始化MD5上下文
     *
     * @return int32_t 成功返回0，失败返回负数
     */
    int32_t init();

    /**
     * @brief 更新MD5数据, 可以多次调用
     *
     * @param data 数据
     * @param len 数据长度
     * @return int32_t 成功返回0, 失败返回负数
     */
    int32_t update(const void *data, int32_t len);
    template<typename T>
    int32_t update(const std::basic_string<T> &data) {
        if (data.empty()) {
            return 0; // No data to update
        }
        return update(data.data(), data.size() * sizeof(T));
    }
    template<typename T>
    int32_t update(const std::vector<T> &data) {
        if (data.empty()) {
            return 0; // No data to update
        }
        return update(data.data(), data.size() * sizeof(T));
    }

    /**
     * @brief 获取MD5结果
     *
     * @param digest 长度为 MD5_DIGEST_LENGTH 的缓冲区，用于存储MD5结果
     * @return int32_t 成功返回0，失败返回负数
     */
    int32_t finalize(std::array<uint8_t, MD5_DIGEST_LENGTH> &digest);
    int32_t finalize(std::vector<uint8_t> &digest);
    std::string finalize();

private:
    std::unique_ptr<MD5Context>     m_context;
};

} // namespace crypto
} // namespace eular

#endif // __CRYPTO_MD5_H__