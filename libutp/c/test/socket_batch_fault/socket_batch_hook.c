#define _GNU_SOURCE

#include "socket_batch_hook.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>

#include <sys/socket.h>

#if defined(UTP_HAVE_SENDMMSG)
static int32_t  g_socket = -1;
static uint32_t g_partial_send_count;
static uint32_t g_intercept_count;
static uint32_t g_last_request_count;
static int (*g_real_sendmmsg)(int, struct mmsghdr*, unsigned int, int);
#endif
#if defined(UTP_HAVE_RECVMMSG)
static int32_t  g_receive_socket = -1;
static uint32_t g_truncated_receive_count;
static int (*g_real_recvmmsg)(int, struct mmsghdr*, unsigned int, int, struct timespec*);
#endif

#if defined(UTP_HAVE_SENDMMSG)
static bool utp_test_batch_hook_load_sendmmsg(void)
{
    void* symbol;

    if (g_real_sendmmsg != NULL) {
        return true;
    }
    symbol = dlsym(RTLD_NEXT, "sendmmsg");
    if (symbol == NULL) {
        return false;
    }
    memcpy(&g_real_sendmmsg, &symbol, sizeof(g_real_sendmmsg));
    return g_real_sendmmsg != NULL;
}
#endif

#if defined(UTP_HAVE_RECVMMSG)
static bool utp_test_batch_hook_load_recvmmsg(void)
{
    void* symbol;

    if (g_real_recvmmsg != NULL) {
        return true;
    }
    symbol = dlsym(RTLD_NEXT, "recvmmsg");
    if (symbol == NULL) {
        return false;
    }
    memcpy(&g_real_recvmmsg, &symbol, sizeof(g_real_recvmmsg));
    return g_real_recvmmsg != NULL;
}
#endif

#if defined(UTP_HAVE_SENDMMSG)
int __wrap_sendmmsg(int socket, struct mmsghdr* messages, unsigned int message_count, int flags)
{
    if (!utp_test_batch_hook_load_sendmmsg()) {
        errno = ENOSYS;
        return -1;
    }
    if (socket == g_socket && g_partial_send_count != 0u && message_count > g_partial_send_count) {
        ++g_intercept_count;
        g_last_request_count = message_count;
        message_count        = g_partial_send_count;
        g_partial_send_count = 0u;
    }
    return g_real_sendmmsg(socket, messages, message_count, flags);
}
#endif

#if defined(UTP_HAVE_RECVMMSG)
int __wrap_recvmmsg(int socket, struct mmsghdr* messages, unsigned int message_count, int flags,
                    struct timespec* timeout)
{
    int received_count;

    if (!utp_test_batch_hook_load_recvmmsg()) {
        errno = ENOSYS;
        return -1;
    }
    if (socket == g_receive_socket && g_truncated_receive_count == 0u && message_count > 2u) {
        message_count = 2u;
    }
    received_count = g_real_recvmmsg(socket, messages, message_count, flags, timeout);
    if (socket == g_receive_socket && g_truncated_receive_count == 0u && received_count >= 2) {
        messages[0].msg_hdr.msg_flags |= MSG_TRUNC;
        ++g_truncated_receive_count;
    }
    return received_count;
}
#endif

#if defined(UTP_HAVE_SENDMMSG)
bool utp_test_batch_hook_configure_partial_send(int32_t native_socket, uint32_t sent_count)
{
    if (native_socket < 0 || sent_count == 0u || !utp_test_batch_hook_load_sendmmsg()) {
        return false;
    }
    g_socket             = native_socket;
    g_partial_send_count = sent_count;
    g_intercept_count    = 0u;
    g_last_request_count = 0u;
    return true;
}

uint32_t utp_test_batch_hook_intercept_count(void) { return g_intercept_count; }

uint32_t utp_test_batch_hook_last_request_count(void) { return g_last_request_count; }
#endif

#if defined(UTP_HAVE_RECVMMSG)
bool     utp_test_batch_hook_configure_truncated_receive(int32_t native_socket)
{
    if (native_socket < 0 || !utp_test_batch_hook_load_recvmmsg()) {
        return false;
    }
    g_receive_socket          = native_socket;
    g_truncated_receive_count = 0u;
    return true;
}

uint32_t utp_test_batch_hook_truncated_receive_count(void) { return g_truncated_receive_count; }
#endif
