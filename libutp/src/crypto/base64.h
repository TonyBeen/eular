/*************************************************************************
    > File Name: base64.h
    > Author: eular
    > Brief:
    > Created Time: Tue 24 Mar 2026 10:00:00 AM CST
 ************************************************************************/

#ifndef __UTP_CRYPTO_BASE64_H__
#define __UTP_CRYPTO_BASE64_H__

#include <string>
#include <vector>

namespace eular {
namespace utp {

class Base64
{
public:
    static bool EncodeStd(const std::vector<uint8_t> &input, std::string &output);
    static bool DecodeStd(const std::string &input, std::vector<uint8_t> &output);
};

} // namespace utp
} // namespace eular

#endif // __UTP_CRYPTO_BASE64_H__