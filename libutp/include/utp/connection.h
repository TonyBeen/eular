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
    using Ptr = std::shared_ptr<Connection>;

    using OnStreamCanCreate = std::function<void()>;
    using OnStreamCreated = std::function<void(Stream *)>;

    struct Description {
        uint32_t        scid;
        uint32_t        dcid;
        std::string     remoteHost;
        uint16_t        remotePort;
    };

    struct Statistic {
        uint32_t        pmtu;
        uint32_t        rtt;
        uint32_t        rttvar;
        uint32_t        bw_estimate;
        uint64_t        rx_bytes;
        uint64_t        tx_bytes;
        uint64_t        rtx_bytes;
    };

    Connection() = default;
    virtual ~Connection() = default;

    virtual void        registerStreamCanCreate(const OnStreamCanCreate &cb) = 0;
    virtual void        registerStreamCreated(const OnStreamCreated &cb) = 0;
    virtual int32_t     streamCount() const = 0;
    virtual Statistic   statistic() const = 0;
    virtual Description description() const = 0;

    virtual int32_t     createStream() = 0;
    virtual void        close() = 0;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONNECTION_H__
