#ifndef __NTRS_IO_H__
#define __NTRS_IO_H__

#include <stdint.h>

#include <ntrs_codec.h>

namespace eular {
namespace ntrs {

bool waitReadable(int fd, int timeout_ms);
bool setSocketTimeoutsMs(int fd, int timeout_ms);
bool connectTcpHostPort(const char* host, uint16_t port, int timeout_ms, int* fd_out);
bool recvMessageWithTimeout(int fd, int timeout_ms, Message* msg);

}  // namespace ntrs
}  // namespace eular

#endif
