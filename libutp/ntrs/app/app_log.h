#ifndef EULAR_NTRS_APP_LOG_H
#define EULAR_NTRS_APP_LOG_H

#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void utp_ntrs_app_log_init(const char* program_name);
int  utp_ntrs_app_log_printf(const char* file, int line, FILE* stream, const char* format, ...);

#ifdef __cplusplus
}
#endif

#define UTP_NTRS_APP_LOG(stream, ...) utp_ntrs_app_log_printf(__FILE__, __LINE__, stream, __VA_ARGS__)

#endif  // EULAR_NTRS_APP_LOG_H
