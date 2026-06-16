#include <ntrs_io.h>

#include <netdb.h>
#include <stdio.h>
#include <string.h>

#include <event2/util.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace eular {
namespace ntrs {

namespace {

static int socketLastError()
{
    return EVUTIL_SOCKET_ERROR();
}

static bool socketInterrupted(int err)
{
#if defined(_WIN32)
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

static bool recvExactTimeout(int fd, void* buf, size_t len, int timeout_ms)
{
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t   used = 0;

    while (used < len) {
        if (!waitReadable(fd, timeout_ms)) {
            return false;
        }

        ssize_t nread = recv(fd, ptr + used, len - used, 0);
        if (nread <= 0) {
            if (nread < 0 && socketInterrupted(socketLastError())) {
                continue;
            }
            return false;
        }

        used += (size_t)nread;
    }

    return true;
}

}  // namespace

bool waitReadable(int fd, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;

    if (fd < 0 || timeout_ms < 0) {
        return false;
    }

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    for (;;) {
        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret > 0) {
            return true;
        }
        if (ret == 0) {
            return false;
        }
        if (!socketInterrupted(socketLastError())) {
            return false;
        }

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
    }
}

bool setSocketTimeoutsMs(int fd, int timeout_ms)
{
    struct timeval tv;

    if (fd < 0 || timeout_ms < 0) {
        return false;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

bool connectTcpHostPort(const char* host, uint16_t port, int timeout_ms, int* fd_out)
{
    struct addrinfo  hints;
    struct addrinfo* result = NULL;
    struct addrinfo* it = NULL;
    char             port_text[16];
    int              fd = -1;

    if (host == NULL || host[0] == '\0' || port == 0 || timeout_ms < 0 || fd_out == NULL) {
        return false;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);

    if (getaddrinfo(host, port_text, &hints, &result) != 0 || result == NULL) {
        return false;
    }

    for (it = result; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            setSocketTimeoutsMs(fd, timeout_ms);
            *fd_out = fd;
            freeaddrinfo(result);
            return true;
        }
        EVUTIL_CLOSESOCKET(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return false;
}

bool recvMessageWithTimeout(int fd, int timeout_ms, Message* msg)
{
    uint8_t  header[FRAME_HDR_SIZE];
    uint8_t  frame[8192];
    uint32_t frame_size = 0;

    if (msg == NULL) {
        return false;
    }
    if (!recvExactTimeout(fd, header, sizeof(header), timeout_ms)) {
        return false;
    }
    if (!frameSizeFromHeader(header, sizeof(header), &frame_size) ||
        frame_size < FRAME_HDR_SIZE || frame_size > sizeof(frame)) {
        return false;
    }

    memcpy(frame, header, sizeof(header));
    if (frame_size > sizeof(header) &&
        !recvExactTimeout(fd, frame + sizeof(header), frame_size - sizeof(header), timeout_ms)) {
        return false;
    }

    return decodeMessage(frame, frame_size, msg);
}

}  // namespace ntrs
}  // namespace eular
