#ifndef EULAR_UTP_CONNECTION_STREAM_H
#define EULAR_UTP_CONNECTION_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/frame.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_stream utp_stream_t;

#define UTP_STREAM_TYPES                 4u
#define UTP_STREAM_CLIENT_INITIATED      0u
#define UTP_STREAM_SERVER_INITIATED      1u
#define UTP_STREAM_UNIDIRECTIONAL        2u
#define UTP_STREAM_RECV_FRAGMENT_LIMIT   16u
#define UTP_STREAM_SEND_ACK_RANGE_LIMIT  16u
#define UTP_STREAM_SEND_BUFFER_CAPACITY  32768u
#define UTP_STREAM_RECV_FRAGMENT_CAP     (UTP_PACKET_MTU_FLOOR - UTP_PACKET_HEADER_SIZE - UTP_FRAME_STREAM_HEADER_SIZE)
#define UTP_STREAM_MAX_RECV_BUFFER_BYTES (2u * 1024u * 1024u)
#define UTP_STREAM_DEFAULT_FLOW_WINDOW   (2u * 1024u * 1024u)

typedef struct utp_stream_recv_fragment {
    uint64_t offset;
    size_t   length;
    size_t   consumed;
    bool     fin;
    uint8_t  data[UTP_STREAM_RECV_FRAGMENT_CAP];
} utp_stream_recv_fragment_t;

typedef struct utp_stream_send_ack_range {
    uint64_t start;
    uint64_t end;
} utp_stream_send_ack_range_t;

struct utp_stream {
    uint32_t                    stream_id;
    uint64_t                    send_buffer_offset;
    uint64_t                    next_send_offset;
    uint64_t                    recv_offset;
    uint64_t                    peer_max_stream_data;
    uint64_t                    local_max_stream_data_advertised;
    size_t                      send_buffer_length;
    size_t                      send_buffer_start;
    size_t                      send_in_flight_bytes;
    size_t                      recv_buffered_bytes;
    size_t                      recv_fragment_count;
    size_t                      send_ack_range_count;
    utp_stream_recv_fragment_t  recv_fragments[UTP_STREAM_RECV_FRAGMENT_LIMIT];
    utp_stream_send_ack_range_t send_ack_ranges[UTP_STREAM_SEND_ACK_RANGE_LIMIT];
    uint8_t                     send_buffer[UTP_STREAM_SEND_BUFFER_CAPACITY];
    bool                        used;
    bool                        local_fin_queued;
    bool                        local_fin_sent;
    bool                        peer_fin;
    bool                        reset;
};

void                 utp_stream_init(utp_stream_t *stream, uint32_t stream_id);
void                 utp_stream_reset(utp_stream_t *stream);
utp_internal_error_t utp_stream_write(utp_stream_t *stream, const uint8_t *data, size_t length, bool fin);
bool                 utp_stream_has_send_work(const utp_stream_t *stream);
utp_internal_error_t utp_stream_build_frame(utp_stream_t *stream, uint8_t *payload, size_t capacity,
                                            size_t *out_payload_length, uint32_t *out_stream_data_size,
                                            uint64_t *out_stream_offset, bool *out_fin);
utp_internal_error_t utp_stream_build_frame_view(utp_stream_t *stream, uint8_t *header, size_t capacity,
                                                 size_t *out_header_length, const uint8_t **out_data,
                                                 uint32_t *out_stream_data_size, uint64_t *out_stream_offset,
                                                 bool *out_fin);
utp_internal_error_t utp_stream_commit_built_frame(utp_stream_t *stream, uint32_t stream_data_size, bool fin);
utp_internal_error_t utp_stream_on_packet_acked(utp_stream_t *stream, uint32_t stream_data_size);
utp_internal_error_t utp_stream_on_packet_acked_range(utp_stream_t *stream, uint64_t stream_offset,
                                                      uint32_t stream_data_size);
void                 utp_stream_update_peer_max_stream_data(utp_stream_t *stream, uint64_t maximum_stream_data);
utp_internal_error_t utp_stream_on_frame(utp_stream_t *stream, const utp_frame_stream_t *frame);
utp_internal_error_t utp_stream_read(utp_stream_t *stream, uint8_t *buffer, size_t capacity, size_t *out_length,
                                     bool *out_fin);
size_t               utp_stream_readable_bytes(const utp_stream_t *stream);
size_t               utp_stream_send_in_flight_bytes(const utp_stream_t *stream);
bool                 utp_stream_is_closed(const utp_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONNECTION_STREAM_H
