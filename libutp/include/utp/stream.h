/*************************************************************************
    > File Name: stream.h
    > Author: eular
    > Brief:
    > Created Time: Tue 13 Jan 2026 05:53:46 PM CST
 ************************************************************************/

#ifndef __UTP_STREAM_H__
#define __UTP_STREAM_H__

#include <memory>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace eular {
namespace utp {
class Stream
{
public:
    static constexpr uint8_t kPriorityHighest = 0;
    static constexpr uint8_t kPriorityLowest = 7;
    static constexpr uint8_t kPriorityDefault = 4;

    struct ConstBufferView {
        const void *data{nullptr};
        size_t      len{0};
    };

    struct MutableBufferView {
        void *data{nullptr};
        size_t len{0};
    };

    enum State : uint8_t {
        kStateOpen = 0,
        kStateHalfClosedLocal,
        kStateHalfClosedRemote,
        kStateClosed,
    };

    using OnReadable = std::function<void()>;
    using OnWritable = std::function<void()>;
    using OnClosed = std::function<void()>;
    using OnReset = std::function<void(uint16_t)>;

    Stream() = default;
    virtual ~Stream() = default;

    virtual uint32_t id() const = 0;
    virtual int32_t write(const void *data, size_t len, bool fin = false) = 0;
    virtual int32_t read(void *buffer, size_t capacity) = 0;
    virtual size_t acquireWriteBuffer(MutableBufferView views[2], size_t maxBytes) = 0;
    virtual int32_t commitWrite(size_t bytes, bool fin = false) = 0;
    virtual size_t acquireReadBuffer(ConstBufferView views[2], size_t maxBytes) const = 0;
    virtual int32_t consumeRead(size_t bytes) = 0;
    virtual State state() const = 0;
    virtual bool readable() const = 0;
    virtual bool writable() const = 0;
    virtual void close() = 0;
    virtual int32_t reset(uint16_t errorCode) = 0;
    virtual bool resetReceived() const = 0;
    virtual int32_t setPriority(uint8_t priority) = 0;
    virtual uint8_t priority() const = 0;

    virtual void setOnReadable(const OnReadable &cb) = 0;
    virtual void setOnWritable(const OnWritable &cb) = 0;
    virtual void setOnClosed(const OnClosed &cb) = 0;
    virtual void setOnReset(const OnReset &cb) = 0;
};

} // namespace utp
} // namespace eular

#endif // __UTP_STREAM_H__
