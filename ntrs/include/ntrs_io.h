#ifndef __NTRS_IO_H__
#define __NTRS_IO_H__

#include <stdint.h>

#include <ntrs_codec.h>

namespace eular {
namespace ntrs {

bool WaitReadable(int fd, int timeout_ms);
bool SetSocketTimeoutsMs(int fd, int timeout_ms);
bool ConnectTcpHostPort(const char* host, uint16_t port, int timeout_ms, int* fd_out);
bool RecvMessageWithTimeout(int fd, int timeout_ms, Message* msg);

}  // namespace ntrs
}  // namespace eular

#endif
