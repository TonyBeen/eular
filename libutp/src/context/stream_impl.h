/*************************************************************************
    > File Name: stream_impl.h
    > Author: eular
    > Brief:
    > Created Time: Wed 28 Jan 2026 05:33:06 PM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_STREAM_IMPL_H__
#define __UTP_CONTEXT_STREAM_IMPL_H__

#include "utp/stream.h"

namespace eular {
namespace utp {
class StreamImpl : public Stream {
public:
    using SP = std::shared_ptr<StreamImpl>;

    StreamImpl();
    ~StreamImpl();
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_STREAM_IMPL_H__
