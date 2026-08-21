#include "app_log.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/syscall.h>

static const char* utp_ntrs_app_log_program = "ntrs";

void               utp_ntrs_app_log_init(const char* program_name)
{
    if (program_name != NULL && program_name[0] != '\0') {
        utp_ntrs_app_log_program = program_name;
    }
}

int utp_ntrs_app_log_printf(const char* file, int line, FILE* stream, const char* format, ...)
{
    char              message[1024];
    char              timestamp[32];
    struct timespec   now;
    struct tm         local_time;
    va_list           arguments;
    char*             content;
    const char*       level     = "I";
    const char* const file_name = strrchr(file, '/');
    int               written;

    va_start(arguments, format);
    written = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0) {
        return written;
    }
    message[strcspn(message, "\r\n")] = '\0';
    content                           = strstr(message, " event=");
    content                           = content == NULL ? message : content + 7u;
    if (strstr(content, "failed") != NULL || strstr(content, "rejected") != NULL ||
        strstr(content, "invalid") != NULL || strstr(content, "overflow") != NULL) {
        level = "E";
    } else if (strstr(content, "dropped") != NULL || strstr(content, "expired") != NULL) {
        level = "W";
    }
    (void)clock_gettime(CLOCK_REALTIME, &now);
    (void)localtime_r(&now.tv_sec, &local_time);
    (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    return fprintf(stream, "%s.%03ld %5ld %s %s: %s    %s:%d\n", timestamp, now.tv_nsec / 1000000L,
                   (long)syscall(SYS_gettid), level, utp_ntrs_app_log_program, content,
                   file_name == NULL ? file : file_name + 1, line);
}
