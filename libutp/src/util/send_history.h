/*************************************************************************
    > File Name: send_history.h
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Feb 2026 11:26:22 AM CST
 ************************************************************************/

#ifndef __UTP_UTIL_SEND_HISTORY_H__
#define __UTP_UTIL_SEND_HISTORY_H__

#include <utp/types.h>

namespace eular {
namespace utp {
class SendHistory
{
public:
    enum {
        SH_WARNED   = 1 << 0,
        SH_GAP_OK   = 1 << 1,
    };

    SendHistory() = default;
    ~SendHistory() = default;

    void            update(utp_packno_t packno);
    utp_packno_t    largestPackNo() const { return _last_sent; }

public:
    utp_packno_t    _last_sent{0};
    utp_packno_t    _warn_thresh{0};
    uint8_t         _flags{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_SEND_HISTORY_H__
