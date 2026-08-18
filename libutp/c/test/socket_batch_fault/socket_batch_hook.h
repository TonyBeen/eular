#ifndef EULAR_UTP_C_TEST_SOCKET_BATCH_HOOK_H
#define EULAR_UTP_C_TEST_SOCKET_BATCH_HOOK_H

#include <stdbool.h>
#include <stdint.h>

bool     utp_test_batch_hook_configure_partial_send(int32_t native_socket, uint32_t sent_count);
uint32_t utp_test_batch_hook_intercept_count(void);
uint32_t utp_test_batch_hook_last_request_count(void);
bool     utp_test_batch_hook_configure_truncated_receive(int32_t native_socket);
uint32_t utp_test_batch_hook_truncated_receive_count(void);

#endif  // EULAR_UTP_C_TEST_SOCKET_BATCH_HOOK_H
