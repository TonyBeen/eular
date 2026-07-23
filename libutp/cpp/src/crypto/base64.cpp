/*************************************************************************
    > File Name: base64.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 24 Mar 2026 10:00:00 AM CST
 ************************************************************************/

#include "crypto/base64.h"

#include <openssl/evp.h>

namespace eular {
namespace utp {

bool Base64::EncodeStd(const std::vector<uint8_t> &input, std::string &output)
{
    if (input.empty()) {
        output.clear();
        return true;
    }

    const size_t outLen = 4 * ((input.size() + 2) / 3);
    output.assign(outLen, '\0');
    const int32_t len = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&output[0]),
                                        reinterpret_cast<const unsigned char *>(input.data()),
                                        static_cast<int32_t>(input.size()));
    if (len <= 0) {
        output.clear();
        return false;
    }

    output.resize(static_cast<size_t>(len));
    return true;
}

bool Base64::DecodeStd(const std::string &input, std::vector<uint8_t> &output)
{
    if (input.empty()) {
        output.clear();
        return true;
    }

    output.assign((input.size() / 4) * 3 + 3, 0);
    const int32_t decodedLen = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(output.data()),
                                               reinterpret_cast<const unsigned char *>(input.data()),
                                               static_cast<int32_t>(input.size()));
    if (decodedLen < 0) {
        output.clear();
        return false;
    }

    size_t padding = 0;
    if (!input.empty() && input.back() == '=') {
        ++padding;
    }
    if (input.size() >= 2 && input[input.size() - 2] == '=') {
        ++padding;
    }

    const size_t realLen = static_cast<size_t>(decodedLen) >= padding
                         ? static_cast<size_t>(decodedLen) - padding
                         : 0;
    output.resize(realLen);
    return true;
}

} // namespace utp
} // namespace eular