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

void utp_connection_set_on_incoming_stream(utp_connection_t* connection, utp_on_incoming_stream_fn callback,
                                           void* user_data)
{
    utp_connection_set_on_incoming_stream_internal(connection, callback, user_data);
}

void utp_connection_set_on_session_token_ready(utp_connection_t* connection, utp_on_session_token_ready_fn callback,
                                               void* user_data)
{
    utp_connection_set_session_token_callback(connection, callback, user_data);
}

utp_status_t utp_connection_export_session_token(const utp_connection_t* connection, uint8_t* buffer, size_t capacity,
                                                 size_t* out_length)
{
    const utp_internal_error_t error =
        utp_connection_export_session_token_internal(connection, buffer, capacity, out_length);

    return error == UTP_INTERNAL_ERROR_NOT_FOUND ? UTP_STATUS_CONNECTION_SESSION_TOKEN_UNAVAILABLE
                                                 : utp_internal_error_to_status(error);
}

int32_t utp_connection_stream_count(const utp_connection_t* connection, utp_stream_type_t type)
{
    return utp_connection_stream_count_internal(connection, type);
}

int32_t utp_connection_creatable_stream_count(const utp_connection_t* connection, utp_stream_type_t type)
{
    return utp_connection_creatable_stream_count_internal(connection, type);
}

utp_status_t utp_connection_get_statistic(const utp_connection_t* connection, utp_connection_statistic_t* out_statistic)
{
    return utp_internal_error_to_status(utp_connection_get_statistic_internal(connection, out_statistic));
}

utp_status_t utp_connection_get_description(const utp_connection_t*       connection,
                                            utp_connection_description_t* out_description)
{
    return utp_internal_error_to_status(utp_connection_get_description_internal(connection, out_description));
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
    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    return utp_public_flush_connection(stream->connection);
}

utp_status_t utp_stream_shutdown(utp_stream_t* stream, utp_stream_shutdown_t how)
{
    utp_internal_error_t error = utp_stream_shutdown_internal(stream, how);

    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    return utp_public_flush_connection(stream->connection);
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
    const utp_internal_error_t error = utp_stream_commit_read_view_internal(stream, offset, length);

    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    return utp_public_flush_connection(stream->connection);
}

void utp_stream_set_on_readable(utp_stream_t* stream, utp_on_stream_readable_fn callback, void* user_data)
{
    utp_stream_set_read_callback(stream, callback, user_data);
}

void utp_stream_set_on_writable(utp_stream_t* stream, utp_on_stream_writable_fn callback, void* user_data)
{
    utp_stream_set_write_callback(stream, callback, user_data);
}

void utp_stream_set_on_closed(utp_stream_t* stream, utp_on_stream_closed_fn callback, void* user_data)
{
    utp_stream_set_close_callback(stream, callback, user_data);
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
