#include <string.h>

#include <ntrs/service.h>

static uint32_t utp_ntrs_control_read_u32(const uint8_t* data)
{
    return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) | ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static bool utp_ntrs_control_type_valid(uint8_t type)
{
    return type >= UTP_NTRS_CONTROL_NODE_REGISTER && type <= UTP_NTRS_CONTROL_NAT_FORWARD_BINDING_RESPONSE;
}

void utp_ntrs_control_stream_init(utp_ntrs_control_stream_t* stream) { *stream = (utp_ntrs_control_stream_t){0}; }

bool utp_ntrs_control_stream_feed(utp_ntrs_control_stream_t* stream, const uint8_t* data, size_t length,
                                  utp_ntrs_control_message_fn callback, void* user_data)
{
    while (length != 0u) {
        size_t copy_length;

        if (stream->expected_length == 0u && stream->used < UTP_NTRS_CONTROL_HEADER_SIZE) {
            copy_length = UTP_NTRS_CONTROL_HEADER_SIZE - stream->used;
            if (copy_length > length) {
                copy_length = length;
            }
            (void)memcpy(stream->buffer + stream->used, data, copy_length);
            stream->used += (uint32_t)copy_length;
            data         += copy_length;
            length       -= copy_length;
            if (stream->used < UTP_NTRS_CONTROL_HEADER_SIZE) {
                continue;
            }
            if (stream->buffer[0] != UTP_NTRS_CONTROL_VERSION || !utp_ntrs_control_type_valid(stream->buffer[1]) ||
                stream->buffer[2] != 0u || stream->buffer[3] != 0u) {
                return false;
            }
            stream->expected_length = UTP_NTRS_CONTROL_HEADER_SIZE + utp_ntrs_control_read_u32(stream->buffer + 4u);
            if (stream->expected_length > UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE) {
                return false;
            }
            if (stream->used == stream->expected_length) {
                callback(user_data, stream->buffer[1], stream->buffer + UTP_NTRS_CONTROL_HEADER_SIZE, 0u);
                utp_ntrs_control_stream_init(stream);
                continue;
            }
        }
        copy_length = stream->expected_length - stream->used;
        if (copy_length > length) {
            copy_length = length;
        }
        (void)memcpy(stream->buffer + stream->used, data, copy_length);
        stream->used += (uint32_t)copy_length;
        data         += copy_length;
        length       -= copy_length;
        if (stream->used == stream->expected_length) {
            callback(user_data, stream->buffer[1], stream->buffer + UTP_NTRS_CONTROL_HEADER_SIZE,
                     stream->expected_length - UTP_NTRS_CONTROL_HEADER_SIZE);
            utp_ntrs_control_stream_init(stream);
        }
    }
    return true;
}
