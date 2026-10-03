#if defined(_WIN32)

#include <stdlib.h>

#include <winsock2.h>

static void utp_test_winsock_cleanup(void)
{
    (void)WSACleanup();
}

static void utp_test_winsock_init(void)
{
    WSADATA data;

    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        abort();
    }
    (void)atexit(utp_test_winsock_cleanup);
}

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void(__cdecl* utp_test_winsock_init_ptr)(void) = utp_test_winsock_init;
#else
__attribute__((constructor)) static void utp_test_winsock_init_ctor(void)
{
    utp_test_winsock_init();
}
#endif

#else

int utp_test_windows_init_unused;

#endif
