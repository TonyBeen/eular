#include <utp/context.h>

#include "connection/connection.h"
#include "connection/stream.h"
#include "context/context.h"
#include "util/error.h"

const char*         utp_version(void) { return UTP_VERSION_STRING; }

static utp_status_t utp_public_flush_connection(utp_connection_t* connection)
{
    if (connection == NULL || connection->context == NULL) {
        return UTP_STATUS_OK;
    }
    return utp_internal_error_to_status(utp_context_flush_public_connection(connection->context, connection));
}

void utp_connection_close(utp_connection_t* connection)
{
    if (utp_connection_queue_close(connection, 0u) == UTP_INTERNAL_ERROR_OK) {
        (void)utp_public_flush_connection(connection);
    }
}

utp_status_t utp_connection_create_stream(utp_connection_t* connection, utp_stream_type_t type, uint32_t* out_stream_id)
{
    utp_internal_error_t error;

    if (type != UTP_STREAM_TYPE_BIDIRECTIONAL && type != UTP_STREAM_TYPE_UNIDIRECTIONAL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    error = utp_connection_create_stream_internal(connection, type == UTP_STREAM_TYPE_BIDIRECTIONAL, out_stream_id);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    return utp_public_flush_connection(connection);
}

utp_stream_t* utp_connection_get_stream(utp_connection_t* connection, uint32_t stream_id)
{
    return utp_connection_find_stream_internal(connection, stream_id);
}

uint32_t utp_stream_id(const utp_stream_t* stream)
{
    return stream == NULL || !stream->used ? UINT32_MAX : stream->stream_id;
}

utp_status_t utp_stream_write(utp_stream_t* stream, const void* data, size_t length)
{
    utp_internal_error_t error;

    error = utp_stream_write_internal(stream, data, length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    return utp_public_flush_connection(stream->connection);
}

utp_status_t utp_stream_read(utp_stream_t* stream, void* buffer, size_t capacity, size_t* out_length)
{
    bool                 fin;
    utp_internal_error_t error;

    error = utp_stream_read_internal(stream, buffer, capacity, out_length, &fin);
    return utp_internal_error_to_status(error);
}

void utp_stream_close(utp_stream_t* stream)
{
    if (utp_stream_close_internal(stream) == UTP_INTERNAL_ERROR_OK && stream != NULL) {
        (void)utp_public_flush_connection(stream->connection);
    }
}

utp_status_t utp_stream_reset(utp_stream_t* stream, uint16_t error_code)
{
    utp_internal_error_t error = utp_stream_reset_internal(stream, error_code);

    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    return utp_public_flush_connection(stream->connection);
}

utp_status_t utp_stream_acquire_write_views(utp_stream_t* stream, utp_stream_write_view_t* views, size_t view_capacity,
                                            size_t* out_view_count, size_t* out_capacity)
{
    return utp_internal_error_to_status(
        utp_stream_acquire_write_views_internal(stream, views, view_capacity, out_view_count, out_capacity));
}

utp_status_t utp_stream_commit_write_views(utp_stream_t* stream, size_t length)
{
    utp_internal_error_t error = utp_stream_commit_write_views_internal(stream, length);

    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    return utp_public_flush_connection(stream->connection);
}

utp_status_t utp_stream_acquire_read_view(utp_stream_t* stream, utp_stream_read_view_t* out_view)
{
    return utp_internal_error_to_status(utp_stream_acquire_read_view_internal(stream, out_view));
}

utp_status_t utp_stream_commit_read_view(utp_stream_t* stream, uint64_t offset, size_t length)
{
    return utp_internal_error_to_status(utp_stream_commit_read_view_internal(stream, offset, length));
}

bool utp_stream_reset_by_peer(const utp_stream_t* stream)
{
    return stream != NULL && stream->used && stream->reset && stream->reset_by_peer;
}

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
