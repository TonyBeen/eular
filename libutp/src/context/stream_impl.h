/*************************************************************************
    > File Name: stream_impl.h
    > Author: eular
    > Brief:
    > Created Time: Wed 28 Jan 2026 05:33:06 PM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_STREAM_IMPL_H__
#define __UTP_CONTEXT_STREAM_IMPL_H__

#include "utp/stream.h"

#define STREAM_TYPES                4 

#define STREAM_ID_MASK              0b11

#define STREAM_ID_IS_CLIENT(ID)     ((ID & 1) == 0)
#define STREAM_ID_IS_SERVER(ID)     ((ID & 1) == 1)
#define STREAM_ID_IS_BI_DIR(ID)     ((ID & 2) == 0)
#define STREAM_ID_IS_UNI_DIR(ID)    ((ID & 2) == 2)

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
