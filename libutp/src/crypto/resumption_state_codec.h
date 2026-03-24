/*************************************************************************
    > File Name: resumption_state_codec.h
    > Author: eular
    > Brief:
    > Created Time: Tue 24 Mar 2026 10:00:00 AM CST
 ************************************************************************/

#ifndef __UTP_CRYPTO_RESUMPTION_STATE_CODEC_H__
#define __UTP_CRYPTO_RESUMPTION_STATE_CODEC_H__

#include <array>
#include <vector>

namespace eular {
namespace utp {

class ResumptionStateCodec
{
public:
    static constexpr size_t KEY_SIZE = 32;
    using Key = std::array<uint8_t, KEY_SIZE>;

    static bool Seal(const Key &key,
                     const std::vector<uint8_t> &plaintext,
                     std::vector<uint8_t> &sealed);

    static bool Open(const Key &key,
                     const std::vector<uint8_t> &sealed,
                     std::vector<uint8_t> &plaintext);
};

} // namespace utp
} // namespace eular

#endif // __UTP_CRYPTO_RESUMPTION_STATE_CODEC_H__