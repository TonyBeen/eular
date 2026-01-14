/*************************************************************************
    > File Name: connection.h
    > Author: eular
    > Brief:
    > Created Time: Tue 13 Jan 2026 05:42:22 PM CST
 ************************************************************************/

#ifndef __UTP_CONNECTION_H__
#define __UTP_CONNECTION_H__

#include <stdint.h>
#include <string>
#include <memory>
#include <functional>

#include <utp/platform.h>
#include <utp/stream.h>

namespace eular {
namespace utp {
class UTP_API Connection
{
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;
public:
    using SP = std::shared_ptr<Connection>;
    using WP = std::weak_ptr<Connection>;
    using Ptr = SP;

    using OnStreamCanCreate = std::function<void()>;
    using OnStreamCreated = std::function<void(Stream *)>;

    struct Description {
        uint32_t        scid;
        uint32_t        dcid;
        std::string     remoteHost;
        uint16_t        remotePort;
    };

    struct Statistic {
        uint32_t        pingTimes;
        uint32_t        pongTimes;
        uint64_t        transmitBytes;
        uint64_t        retransmitBytes;
        int32_t         srtt;   // smoothed round trip time (us)
        int32_t         rttvar; // round trip time variance (us)
        int32_t         rto;    // retransmission timeout (us)
    };

    Connection() = default;
    virtual ~Connection() = default;

    virtual int32_t createStream() = 0;
    virtual int32_t streamCount() const = 0;
    virtual Statistic statistic() const = 0;
    virtual void close() = 0;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONNECTION_H__
