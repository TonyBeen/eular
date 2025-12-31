/*************************************************************************
    > File Name: crypto_utils.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月07日 星期一 11时39分45秒
 ************************************************************************/

#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <string>

namespace utils {
static inline std::string Hex2String(const void *data, int32_t size)
{
    const uint8_t *hex = static_cast<const uint8_t *>(data);
    std::string result;
    result.reserve(2 * size);
    for (int32_t i = 0; i < size; i++) {
        static const char dec2hex[16+1] = "0123456789abcdef";
        result += dec2hex[(hex[i] >> 4) & 15];
        result += dec2hex[ hex[i]       & 15];
    }

    return result;
}

} // namespace utils
#endif // CRYPTO_UTILS_H
