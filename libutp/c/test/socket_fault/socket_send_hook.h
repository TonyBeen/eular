#ifndef EULAR_UTP_C_TEST_SOCKET_SEND_HOOK_H
#define EULAR_UTP_C_TEST_SOCKET_SEND_HOOK_H

#include <stdbool.h>
#include <stdint.h>

bool     utp_test_send_hook_configure(int32_t native_socket, int32_t system_error, uint32_t count);
uint32_t utp_test_send_hook_remaining(void);

#endif  // EULAR_UTP_C_TEST_SOCKET_SEND_HOOK_H
