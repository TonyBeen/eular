#include "socket_send_hook.h"

#include <errno.h>
#include <fishhook.h>

#include <sys/socket.h>

static int32_t  g_socket = -1;
static int32_t  g_error;
static uint32_t g_remaining;
static int      g_rebound;
static ssize_t (*g_real_sendto)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
static ssize_t (*g_real_sendmsg)(int, const struct msghdr*, int);

static int utp_test_send_hook_should_fail(int socket)
{
    if (socket != g_socket || g_remaining == 0u) {
        return 0;
    }
    --g_remaining;
    errno = g_error;
    return 1;
}

static ssize_t utp_test_sendto(int socket, const void* data, size_t length, int flags, const struct sockaddr* address,
                               socklen_t address_length)
{
    return utp_test_send_hook_should_fail(socket) ? -1
                                                  : g_real_sendto(socket, data, length, flags, address, address_length);
}

static ssize_t utp_test_sendmsg(int socket, const struct msghdr* message, int flags)
{
    return utp_test_send_hook_should_fail(socket) ? -1 : g_real_sendmsg(socket, message, flags);
}

bool utp_test_send_hook_configure(int32_t native_socket, int32_t system_error, uint32_t count)
{
    if (!g_rebound) {
        struct rebinding bindings[] = {{"sendto", (void*)utp_test_sendto, (void**)&g_real_sendto},
                                       {"sendmsg", (void*)utp_test_sendmsg, (void**)&g_real_sendmsg}};

        if (rebind_symbols(bindings, sizeof(bindings) / sizeof(bindings[0])) != 0 || g_real_sendto == NULL ||
            g_real_sendmsg == NULL) {
            return false;
        }
        g_rebound = 1;
    }
    g_socket    = native_socket;
    g_error     = system_error;
    g_remaining = count;
    return true;
}

uint32_t utp_test_send_hook_remaining(void) { return g_remaining; }
