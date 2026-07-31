#include <utp/utp.h>

#include "connection/stream.h"

const char* utp_version(void) { return UTP_VERSION_STRING; }

utp_status_t utp_stream_set_priority(utp_stream_t* stream, uint8_t priority)
{
    if (stream == NULL || !stream->used || priority > UTP_STREAM_PRIORITY_LOWEST) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    stream->priority = priority;
    return UTP_STATUS_OK;
}

uint8_t utp_stream_priority(const utp_stream_t* stream)
{
    return stream == NULL || !stream->used ? UTP_STREAM_PRIORITY_DEFAULT : stream->priority;
}
