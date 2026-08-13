#define CATCH_CONFIG_MAIN

#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch.hpp>

extern "C" {
#include <utp/context.h>

#include "connection/connection.h"
}

namespace {

utp_address_t loopback_address(uint16_t port)
{
    utp_address_t address = {};

    REQUIRE(utp_address_parse(&address, "127.0.0.1", port) == UTP_INTERNAL_ERROR_OK);
    return address;
}

utp_packet_in_t* stream_frame_packet(utp_packet_in_pool_t* pool, uint32_t stream_id, uint64_t offset, const char* data,
                                     bool fin, utp_frame_stream_t* decoded)
{
    utp_packet_in_t*         packet = nullptr;
    const size_t             length = std::strlen(data);
    const utp_frame_stream_t frame = {static_cast<uint8_t>(fin ? UTP_STREAM_FLAG_FIN : UTP_STREAM_FLAG_NONE), stream_id,
                                      offset, reinterpret_cast<const uint8_t*>(data), static_cast<uint16_t>(length)};

    REQUIRE(decoded != nullptr);
    REQUIRE(utp_packet_in_pool_acquire(pool, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_stream_encode(packet->data, packet->capacity, &frame) == UTP_INTERNAL_ERROR_OK);
    packet->length = UTP_FRAME_STREAM_HEADER_SIZE + frame.data_length;
    REQUIRE(utp_frame_stream_decode(decoded, packet->data, packet->length) == UTP_INTERNAL_ERROR_OK);
    return packet;
}

void transfer_next_packet(utp_connection_t* sender, utp_connection_t* receiver, const utp_address_t* sender_address,
                          uint64_t now_us, utp_packet_in_pool_t* receive_pool)
{
    utp_packet_out_t* packet = utp_connection_next_packet_to_send(sender);
    utp_packet_in_t*  wire   = nullptr;
    size_t            length;

    REQUIRE(packet != nullptr);
    REQUIRE(utp_packet_in_pool_acquire(receive_pool, &wire) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_flatten(packet, wire->data, wire->capacity, &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == packet->data_size);
    wire->length = static_cast<uint16_t>(length);
    REQUIRE(utp_connection_on_packet_sent(sender, packet, now_us) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_in_received(receiver, wire, sender_address, now_us) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_release(wire);
}

utp_internal_error_t transfer_control_payload(utp_connection_t* sender, utp_connection_t* receiver,
                                              const utp_address_t* sender_address, const uint8_t* payload,
                                              size_t payload_length, uint64_t now_us,
                                              utp_packet_in_pool_t* receive_pool)
{
    utp_packet_out_t*    packet;
    utp_packet_in_t*     wire = nullptr;
    size_t               length;
    utp_internal_error_t error;

    REQUIRE(utp_connection_queue_packet(sender, UTP_PACKET_TYPE_CTRL, payload, payload_length, false) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(sender);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_packet_in_pool_acquire(receive_pool, &wire) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_flatten(packet, wire->data, wire->capacity, &length) == UTP_INTERNAL_ERROR_OK);
    wire->length = static_cast<uint16_t>(length);
    REQUIRE(utp_connection_on_packet_sent(sender, packet, now_us) == UTP_INTERNAL_ERROR_OK);
    error = utp_connection_on_packet_in_received(receiver, wire, sender_address, now_us);
    utp_packet_in_release(wire);
    return error;
}

const uint8_t* first_packet_frame(utp_packet_out_t* packet, uint8_t* out_frame_type, size_t* out_frame_length)
{
    utp_packet_view_t view         = {};
    size_t            offset       = 0u;
    const uint8_t*    frame        = nullptr;
    uint8_t           frame_type   = 0u;
    size_t            frame_length = 0u;

    REQUIRE(packet != nullptr);
    REQUIRE(out_frame_type != nullptr);
    REQUIRE(out_frame_length != nullptr);
    REQUIRE(utp_packet_view_decode(&view, packet->raw_data, packet->data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    *out_frame_type   = frame_type;
    *out_frame_length = frame_length;
    return frame;
}

const uint8_t* packet_frame_of_type(utp_packet_out_t* packet, uint8_t requested_type, size_t* out_frame_length)
{
    utp_packet_view_t view   = {};
    size_t            offset = 0u;

    REQUIRE(packet != nullptr);
    REQUIRE(out_frame_length != nullptr);
    REQUIRE(utp_packet_view_decode(&view, packet->raw_data, packet->data_size) == UTP_INTERNAL_ERROR_OK);
    while (offset < view.payload_length) {
        const uint8_t* frame        = nullptr;
        uint8_t        frame_type   = UTP_FRAME_TYPE_INVALID;
        size_t         frame_length = 0u;

        REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) ==
                UTP_INTERNAL_ERROR_OK);
        if (frame_type == requested_type) {
            *out_frame_length = frame_length;
            return frame;
        }
    }
    return nullptr;
}

struct stream_callback_probe {
    int32_t readable_count;
    int32_t writable_count;
    int32_t closed_count;
};

struct incoming_terminal_probe {
    int32_t incoming_count;
    int32_t closed_count;
    int32_t writable_count;
    bool    reset_visible;
    bool    read_cancelled;
    bool    write_cancelled;
    bool    inside_incoming;
    bool    writable_during_incoming;
};

void on_stream_readable(utp_stream_t* stream, void* user_data)
{
    auto* probe = static_cast<stream_callback_probe*>(user_data);

    REQUIRE(stream != nullptr);
    ++probe->readable_count;
}

void on_stream_writable(utp_stream_t* stream, void* user_data)
{
    auto* probe = static_cast<stream_callback_probe*>(user_data);

    REQUIRE(stream != nullptr);
    ++probe->writable_count;
}

void on_stream_closed(utp_stream_t* stream, void* user_data)
{
    auto* probe = static_cast<stream_callback_probe*>(user_data);

    REQUIRE(stream != nullptr);
    ++probe->closed_count;
}

void on_incoming_terminal_closed(utp_stream_t* stream, void* user_data)
{
    auto* probe = static_cast<incoming_terminal_probe*>(user_data);

    REQUIRE(stream != nullptr);
    ++probe->closed_count;
}

void on_incoming_terminal(utp_connection_t* connection, utp_stream_t* stream, void* user_data)
{
    auto*   probe = static_cast<incoming_terminal_probe*>(user_data);
    uint8_t byte;
    size_t  length = 0u;

    REQUIRE(connection != nullptr);
    REQUIRE(stream != nullptr);
    ++probe->incoming_count;
    probe->reset_visible = stream->peer_reset;
    probe->read_cancelled =
        utp_stream_read(stream, &byte, sizeof(byte), &length) == UTP_STATUS_CANCELLED && length == 0u;
    utp_stream_set_on_closed(stream, on_incoming_terminal_closed, probe);
    REQUIRE(utp_stream_reset(stream, UTP_PROTOCOL_STOP_SENDING_CANCELLED) == UTP_STATUS_OK);
}

void on_incoming_stopped(utp_connection_t* connection, utp_stream_t* stream, void* user_data)
{
    auto* probe = static_cast<incoming_terminal_probe*>(user_data);

    REQUIRE(connection != nullptr);
    REQUIRE(stream != nullptr);
    ++probe->incoming_count;
    probe->write_cancelled =
        utp_stream_write(stream, "x", 1u) == UTP_STATUS_CANCELLED && stream->peer_stop_sending_received;
}

void on_incoming_writable(utp_stream_t* stream, void* user_data)
{
    auto* probe = static_cast<incoming_terminal_probe*>(user_data);

    REQUIRE(stream != nullptr);
    ++probe->writable_count;
    probe->writable_during_incoming = probe->inside_incoming;
}

void on_incoming_register_writable(utp_connection_t* connection, utp_stream_t* stream, void* user_data)
{
    auto* probe = static_cast<incoming_terminal_probe*>(user_data);

    REQUIRE(connection != nullptr);
    REQUIRE(stream != nullptr);
    ++probe->incoming_count;
    probe->inside_incoming = true;
    utp_stream_set_on_writable(stream, on_incoming_writable, probe);
    probe->inside_incoming = false;
}

}  // namespace

TEST_CASE("stream callbacks notify readable writable closed and reset state", "[stream][callback]")
{
    utp_packet_in_pool_t  pool                                       = {};
    utp_stream_t          receive                                    = {};
    utp_stream_t          writable                                   = {};
    utp_frame_stream_t    frame                                      = {};
    utp_packet_in_t*      packet                                     = nullptr;
    stream_callback_probe receive_probe                              = {};
    stream_callback_probe writable_probe                             = {};
    uint8_t               payload[UTP_FRAME_STREAM_HEADER_SIZE + 1u] = {};
    uint8_t               read_buffer[8]                             = {};
    size_t                payload_length                             = 0u;
    size_t                read_length                                = 0u;
    uint32_t              stream_data_size                           = 0u;
    uint64_t              stream_offset                              = 0u;
    bool                  fin                                        = false;
    std::vector<uint8_t>  write_data(UTP_STREAM_SEND_BUFFER_CAPACITY, 0x5au);

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&receive, 0u);
    utp_stream_set_on_readable(&receive, on_stream_readable, &receive_probe);
    utp_stream_set_on_closed(&receive, on_stream_closed, &receive_probe);
    packet = stream_frame_packet(&pool, 0u, 0u, "hello", true, &frame);
    REQUIRE(utp_stream_on_frame_packet(&receive, &frame, packet) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_release(packet);
    REQUIRE(receive_probe.readable_count == 1);
    REQUIRE(receive_probe.closed_count == 0);
    REQUIRE(utp_stream_close_internal(&receive) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_build_frame(&receive, payload, sizeof(payload), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size == 0u);
    REQUIRE(fin);
    REQUIRE(utp_stream_commit_built_frame(&receive, stream_data_size, fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_read_internal(&receive, read_buffer, sizeof(read_buffer), &read_length, &fin) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(read_length == 5u);
    REQUIRE(receive_probe.closed_count == 1);

    utp_stream_init(&writable, 0u);
    REQUIRE(utp_stream_write_internal(&writable, write_data.data(), write_data.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_build_frame(&writable, payload, sizeof(payload), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size != 0u);
    REQUIRE(utp_stream_commit_built_frame(&writable, stream_data_size, fin) == UTP_INTERNAL_ERROR_OK);
    utp_stream_set_on_writable(&writable, on_stream_writable, &writable_probe);
    REQUIRE(writable_probe.writable_count == 0);
    REQUIRE(utp_stream_on_packet_acked_range(&writable, stream_offset, stream_data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(writable_probe.writable_count == 1);

    utp_stream_cleanup(&receive);
    utp_stream_cleanup(&writable);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("stream reassembles out-of-order frames and reports FIN after data is consumed", "[stream]")
{
    utp_packet_in_pool_t pool       = {};
    utp_stream_t         stream     = {};
    uint8_t              buffer[16] = {};
    size_t               length     = 0u;
    bool                 fin        = false;

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 2u, 128u) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
    REQUIRE(utp_stream_read_internal(&stream, buffer, sizeof(buffer), &length, &fin) == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    REQUIRE(length == 0u);
    REQUIRE_FALSE(fin);
    {
        utp_frame_stream_t tail    = {};
        utp_frame_stream_t head    = {};
        utp_packet_in_t*   tail_in = stream_frame_packet(&pool, 0u, 5u, "world", true, &tail);
        utp_packet_in_t*   head_in = stream_frame_packet(&pool, 0u, 0u, "hello", false, &head);

        REQUIRE(utp_stream_on_frame_packet(&stream, &tail, tail_in) == UTP_INTERNAL_ERROR_OK);
        utp_packet_in_release(tail_in);
        REQUIRE(utp_stream_readable_bytes(&stream) == 0u);
        REQUIRE(utp_stream_on_frame_packet(&stream, &head, head_in) == UTP_INTERNAL_ERROR_OK);
        utp_packet_in_release(head_in);
    }
    REQUIRE(utp_stream_readable_bytes(&stream) == 10u);
    REQUIRE(utp_stream_read_internal(&stream, buffer, sizeof(buffer), &length, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 10u);
    REQUIRE(std::memcmp(buffer, "helloworld", 10u) == 0);
    REQUIRE_FALSE(fin);
    REQUIRE(utp_stream_read_internal(&stream, buffer, sizeof(buffer), &length, &fin) == UTP_INTERNAL_ERROR_CLOSED);
    REQUIRE(length == 0u);
    REQUIRE(fin);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("stream rejects data and RESET beyond a FIN final offset", "[stream][fin][reset]")
{
    utp_packet_in_pool_t pool      = {};
    utp_stream_t         stream    = {};
    utp_frame_stream_t   fin_frame = {};
    utp_frame_stream_t   overflow  = {};
    utp_packet_in_t*     fin_packet;
    utp_packet_in_t*     overflow_packet;

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 2u, 128u) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
    fin_packet = stream_frame_packet(&pool, 0u, 0u, "abc", true, &fin_frame);
    REQUIRE(utp_stream_on_frame_packet(&stream, &fin_frame, fin_packet) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_release(fin_packet);
    REQUIRE(stream.peer_final_size_known);
    REQUIRE(stream.peer_final_size == 3u);

    overflow_packet = stream_frame_packet(&pool, 0u, 3u, "d", false, &overflow);
    REQUIRE(utp_stream_on_frame_packet(&stream, &overflow, overflow_packet) == UTP_INTERNAL_ERROR_PROTOCOL);
    utp_packet_in_release(overflow_packet);
    REQUIRE(utp_stream_on_peer_reset(&stream, 4u) == UTP_INTERNAL_ERROR_PROTOCOL);
    REQUIRE(utp_stream_on_peer_reset(&stream, 3u) == UTP_INTERNAL_ERROR_OK);

    utp_stream_cleanup(&stream);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("stream packet-backed fragments keep packet_in referenced until consumed", "[stream][packet_in]")
{
    utp_packet_in_pool_t     pool       = {};
    utp_packet_in_t*         packet     = nullptr;
    utp_stream_t             stream     = {};
    uint8_t                  buffer[16] = {};
    size_t                   length     = 0u;
    bool                     fin        = false;
    const char*              data       = "hello";
    const utp_frame_stream_t frame      = {UTP_STREAM_FLAG_NONE, 0u, 0u, reinterpret_cast<const uint8_t*>(data), 5u};
    utp_frame_stream_t       decoded    = {};

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_stream_encode(packet->data, packet->capacity, &frame) == UTP_INTERNAL_ERROR_OK);
    packet->length = UTP_FRAME_STREAM_HEADER_SIZE + frame.data_length;
    REQUIRE(utp_frame_stream_decode(&decoded, packet->data, packet->length) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
    REQUIRE(utp_stream_on_frame_packet(&stream, &decoded, packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet->ref_count == 2u);
    utp_packet_in_release(packet);
    REQUIRE(packet->in_use);
    REQUIRE(packet->ref_count == 1u);
    REQUIRE(utp_stream_read_internal(&stream, buffer, sizeof(buffer), &length, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 5u);
    REQUIRE(std::memcmp(buffer, data, length) == 0);
    REQUIRE_FALSE(fin);
    REQUIRE_FALSE(packet->in_use);
    REQUIRE(packet->ref_count == 0u);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("stream packet-backed fragments accept payloads larger than the 1280 MTU floor", "[stream][packet_in]")
{
    static constexpr size_t  payload_length = 1464u;
    utp_packet_in_pool_t     pool           = {};
    utp_packet_in_t*         packet         = nullptr;
    utp_stream_t             stream         = {};
    std::vector<uint8_t>     payload(payload_length, 0xabu);
    const utp_frame_stream_t frame   = {UTP_STREAM_FLAG_NONE, 0u, 0u, payload.data(),
                                        static_cast<uint16_t>(payload.size())};
    utp_frame_stream_t       decoded = {};
    utp_stream_read_view_t   view    = {};

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 2048u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_stream_encode(packet->data, packet->capacity, &frame) == UTP_INTERNAL_ERROR_OK);
    packet->length = UTP_FRAME_STREAM_HEADER_SIZE + frame.data_length;
    REQUIRE(utp_frame_stream_decode(&decoded, packet->data, packet->length) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
    REQUIRE(utp_stream_on_frame_packet(&stream, &decoded, packet) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_release(packet);
    REQUIRE(utp_stream_acquire_read_view_internal(&stream, &view) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.length == payload.size());
    REQUIRE(view.data == decoded.data);
    REQUIRE(std::memcmp(view.data, payload.data(), payload.size()) == 0);
    REQUIRE(utp_stream_commit_read_view_internal(&stream, view.offset, view.length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE_FALSE(packet->in_use);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("stream read views consume packet-backed fragments without copying", "[stream][read_view][packet_in]")
{
    utp_packet_in_pool_t     pool     = {};
    utp_packet_in_t*         packet   = nullptr;
    utp_stream_t             stream   = {};
    const char*              data     = "hello";
    const utp_frame_stream_t frame    = {UTP_STREAM_FLAG_FIN, 0u, 0u, reinterpret_cast<const uint8_t*>(data), 5u};
    utp_frame_stream_t       decoded  = {};
    utp_stream_read_view_t   view     = {};
    utp_stream_read_view_t   fin_view = {};

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_stream_encode(packet->data, packet->capacity, &frame) == UTP_INTERNAL_ERROR_OK);
    packet->length = UTP_FRAME_STREAM_HEADER_SIZE + frame.data_length;
    REQUIRE(utp_frame_stream_decode(&decoded, packet->data, packet->length) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
    REQUIRE(utp_stream_on_frame_packet(&stream, &decoded, packet) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_release(packet);
    REQUIRE(packet->in_use);
    REQUIRE(packet->ref_count == 1u);

    REQUIRE(utp_stream_acquire_read_view_internal(&stream, &view) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.offset == 0u);
    REQUIRE(view.length == 5u);
    REQUIRE_FALSE(view.fin);
    REQUIRE(view.data == decoded.data);
    REQUIRE(std::memcmp(view.data, data, view.length) == 0);
    REQUIRE(utp_stream_commit_read_view_internal(&stream, view.offset, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet->in_use);
    REQUIRE(packet->ref_count == 1u);

    REQUIRE(utp_stream_acquire_read_view_internal(&stream, &view) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.offset == 2u);
    REQUIRE(view.length == 3u);
    REQUIRE(std::memcmp(view.data, data + 2u, view.length) == 0);
    REQUIRE(utp_stream_commit_read_view_internal(&stream, view.offset, view.length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE_FALSE(packet->in_use);
    REQUIRE(packet->ref_count == 0u);

    REQUIRE(utp_stream_acquire_read_view_internal(&stream, &fin_view) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(fin_view.offset == 5u);
    REQUIRE(fin_view.length == 0u);
    REQUIRE(fin_view.data == nullptr);
    REQUIRE(fin_view.fin);
    REQUIRE(utp_stream_commit_read_view_internal(&stream, fin_view.offset, fin_view.length) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("stream write credit includes in-flight bytes until ACKed", "[stream]")
{
    utp_stream_t                                                                        stream           = {};
    std::array<uint8_t, UTP_STREAM_SEND_BUFFER_CAPACITY>                                data             = {};
    std::array<uint8_t, UTP_STREAM_SEND_BUFFER_CAPACITY + UTP_FRAME_STREAM_HEADER_SIZE> payload          = {};
    size_t                                                                              payload_length   = 0u;
    uint32_t                                                                            stream_data_size = 0u;
    uint64_t                                                                            stream_offset    = 0u;
    bool                                                                                fin              = false;

    utp_stream_init(&stream, 0u);
    REQUIRE(utp_stream_write_internal(&stream, data.data(), data.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_build_frame(&stream, payload.data(), payload.size(), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size == data.size());
    REQUIRE_FALSE(fin);
    REQUIRE(utp_stream_commit_built_frame(&stream, stream_data_size, fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_send_in_flight_bytes(&stream) == data.size());
    REQUIRE(utp_stream_write_internal(&stream, data.data(), 1u) == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    REQUIRE(utp_stream_on_packet_acked(&stream, stream_data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_send_in_flight_bytes(&stream) == 0u);
    REQUIRE(utp_stream_write_internal(&stream, data.data(), 1u) == UTP_INTERNAL_ERROR_OK);
}

TEST_CASE("stream write views commit caller-filled data without an extra copy", "[stream][write_view]")
{
    utp_stream_t            stream                               = {};
    utp_stream_write_view_t views[2]                             = {};
    size_t                  view_count                           = 0u;
    size_t                  writable                             = 0u;
    uint8_t                 header[UTP_FRAME_STREAM_HEADER_SIZE] = {};
    size_t                  header_length                        = 0u;
    const uint8_t*          stream_data                          = nullptr;
    uint32_t                stream_data_size                     = 0u;
    uint64_t                stream_offset                        = 0u;
    bool                    fin                                  = false;

    utp_stream_init(&stream, 0u);
    REQUIRE(utp_stream_acquire_write_views_internal(&stream, views, 2u, &view_count, &writable) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(view_count == 1u);
    REQUIRE(writable == UTP_STREAM_SEND_BUFFER_CAPACITY);
    REQUIRE(views[0].data == stream.send_buffer);
    REQUIRE(views[0].length == UTP_STREAM_SEND_BUFFER_CAPACITY);
    std::memcpy(views[0].data, "abc", 3u);
    REQUIRE(utp_stream_commit_write_views_internal(&stream, 3u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_close_internal(&stream) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(&stream, reinterpret_cast<const uint8_t*>("d"), 1u) == UTP_INTERNAL_ERROR_CLOSED);
    REQUIRE(utp_stream_build_frame_view(&stream, header, sizeof(header), &header_length, &stream_data,
                                        &stream_data_size, &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(header_length == UTP_FRAME_STREAM_HEADER_SIZE);
    REQUIRE(stream_data == views[0].data);
    REQUIRE(stream_data_size == 3u);
    REQUIRE(stream_offset == 0u);
    REQUIRE(fin);
    REQUIRE(std::memcmp(stream_data, "abc", 3u) == 0);
}

TEST_CASE("stream write views expose both ring segments when free space wraps", "[stream][write_view]")
{
    utp_stream_t            stream     = {};
    utp_stream_write_view_t views[2]   = {};
    size_t                  view_count = 0u;
    size_t                  writable   = 0u;

    utp_stream_init(&stream, 0u);
    stream.send_buffer_start = UTP_STREAM_SEND_BUFFER_CAPACITY - 2u;
    REQUIRE(utp_stream_acquire_write_views_internal(&stream, views, 2u, &view_count, &writable) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(view_count == 2u);
    REQUIRE(writable == UTP_STREAM_SEND_BUFFER_CAPACITY);
    REQUIRE(views[0].data == stream.send_buffer + UTP_STREAM_SEND_BUFFER_CAPACITY - 2u);
    REQUIRE(views[0].length == 2u);
    REQUIRE(views[1].data == stream.send_buffer);
    REQUIRE(views[1].length == UTP_STREAM_SEND_BUFFER_CAPACITY - 2u);
}

TEST_CASE("stream close sends an empty FIN and keeps the read side open", "[stream][close]")
{
    utp_packet_in_pool_t    pool                                 = {};
    utp_stream_t            stream                               = {};
    utp_stream_write_view_t write_view                           = {};
    utp_frame_stream_t      decoded                              = {};
    utp_packet_in_t*        packet                               = nullptr;
    uint8_t                 header[UTP_FRAME_STREAM_HEADER_SIZE] = {};
    uint8_t                 buffer[8]                            = {};
    const uint8_t*          stream_data                          = nullptr;
    size_t                  header_length                        = 0u;
    uint32_t                stream_data_size                     = 0u;
    uint64_t                stream_offset                        = 0u;
    size_t                  view_count                           = 0u;
    size_t                  writable                             = 0u;
    size_t                  read_length                          = 0u;
    bool                    fin                                  = false;

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
    REQUIRE(utp_stream_close_internal(&stream) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_close_internal(&stream) == UTP_INTERNAL_ERROR_CLOSED);
    REQUIRE(utp_stream_acquire_write_views_internal(&stream, &write_view, 1u, &view_count, &writable) ==
            UTP_INTERNAL_ERROR_CLOSED);
    REQUIRE(utp_stream_build_frame_view(&stream, header, sizeof(header), &header_length, &stream_data,
                                        &stream_data_size, &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size == 0u);
    REQUIRE(stream_offset == 0u);
    REQUIRE(fin);

    packet = stream_frame_packet(&pool, 0u, 0u, "peer", false, &decoded);
    REQUIRE(utp_stream_on_frame_packet(&stream, &decoded, packet) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_release(packet);
    REQUIRE(utp_stream_read_internal(&stream, buffer, sizeof(buffer), &read_length, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(read_length == 4u);
    REQUIRE(std::memcmp(buffer, "peer", 4u) == 0);
    REQUIRE_FALSE(fin);
    REQUIRE(utp_stream_read_internal(&stream, buffer, sizeof(buffer), &read_length, &fin) ==
            UTP_INTERNAL_ERROR_WOULD_BLOCK);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("stream send and receive paths enforce stream-level flow-control limits", "[stream][flow]")
{
    utp_stream_t            stream           = {};
    std::array<uint8_t, 64> payload          = {};
    size_t                  payload_length   = 0u;
    uint32_t                stream_data_size = 0u;
    uint64_t                stream_offset    = 0u;
    bool                    fin              = false;

    utp_stream_init(&stream, 0u);
    stream.peer_max_stream_data = 2u;
    REQUIRE(utp_stream_write_internal(&stream, reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_build_frame(&stream, payload.data(), payload.size(), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size == 2u);
    REQUIRE(utp_stream_commit_built_frame(&stream, stream_data_size, fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_build_frame(&stream, payload.data(), payload.size(), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    utp_stream_update_peer_max_stream_data(&stream, 3u);
    REQUIRE(utp_stream_build_frame(&stream, payload.data(), payload.size(), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size == 1u);

    stream.local_max_stream_data_advertised = 1u;
    {
        utp_packet_in_pool_t pool    = {};
        utp_frame_stream_t   decoded = {};
        utp_packet_in_t*     packet;

        REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
        packet = stream_frame_packet(&pool, 0u, 0u, "ab", false, &decoded);
        REQUIRE(utp_stream_on_frame_packet(&stream, &decoded, packet) == UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL);
        REQUIRE(utp_internal_error_to_status(UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL) == UTP_STATUS_STREAM_FLOW_CONTROL);
        utp_packet_in_release(packet);
        utp_packet_in_pool_cleanup(&pool);
    }
}

TEST_CASE("stream rejects malformed peer frame fields as protocol errors", "[stream][protocol]")
{
    utp_packet_in_pool_t pool   = {};
    utp_packet_in_t*     packet = nullptr;
    utp_stream_t         stream = {};
    utp_frame_stream_t   frame  = {};

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &packet) == UTP_INTERNAL_ERROR_OK);

    frame.flags       = UINT8_C(0x80);
    frame.stream_id   = 0u;
    frame.offset      = 0u;
    frame.data        = reinterpret_cast<const uint8_t*>("x");
    frame.data_length = 1u;
    REQUIRE(utp_stream_on_frame_packet(&stream, &frame, packet) == UTP_INTERNAL_ERROR_PROTOCOL);

    frame.flags       = UTP_STREAM_FLAG_NONE;
    frame.data        = nullptr;
    frame.data_length = 0u;
    REQUIRE(utp_stream_on_frame_packet(&stream, &frame, packet) == UTP_INTERNAL_ERROR_PROTOCOL);

    utp_packet_in_release(packet);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("stream rejects excessive receive gap without copying packet data", "[stream][flow]")
{
    utp_packet_in_pool_t pool    = {};
    utp_frame_stream_t   decoded = {};
    utp_packet_in_t*     packet;
    utp_stream_t         stream = {};

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
    stream.local_max_stream_data_advertised = UINT64_C(4) * UINT64_C(1024) * UINT64_C(1024);

    packet = stream_frame_packet(&pool, 0u, (uint64_t)UTP_STREAM_RECV_MAX_GAP + 1u, "x", false, &decoded);
    REQUIRE(utp_stream_on_frame_packet(&stream, &decoded, packet) == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    REQUIRE(stream.recv_fragment_count == 0u);

    utp_packet_in_release(packet);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("stream rolls back partial receive fragments when a split frame hits memory limit", "[stream][flow]")
{
    utp_packet_in_pool_t pool        = {};
    utp_stream_t         stream      = {};
    utp_frame_stream_t   middle      = {};
    utp_frame_stream_t   full        = {};
    utp_packet_in_t*     middle_in   = nullptr;
    utp_packet_in_t*     full_in     = nullptr;
    const size_t         new_cost    = sizeof(utp_stream_recv_fragment_t) + 128u;
    const size_t         pinned_base = UTP_STREAM_RECV_REASSEMBLY_MEMORY_LIMIT - new_cost;

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 2u, 128u) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);

    middle_in = stream_frame_packet(&pool, 0u, 1u, "b", false, &middle);
    REQUIRE(utp_stream_on_frame_packet(&stream, &middle, middle_in) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_release(middle_in);
    REQUIRE(stream.recv_fragment_count == 1u);
    REQUIRE(stream.recv_fragments[0].offset == 1u);

    stream.recv_pinned_memory_bytes = pinned_base;
    full_in                         = stream_frame_packet(&pool, 0u, 0u, "abc", false, &full);
    REQUIRE(utp_stream_on_frame_packet(&stream, &full, full_in) == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    REQUIRE(stream.recv_fragment_count == 1u);
    REQUIRE(stream.recv_fragments[0].offset == 1u);
    REQUIRE(stream.recv_fragments[0].length == 1u);
    REQUIRE(stream.recv_pinned_memory_bytes == pinned_base);

    utp_packet_in_release(full_in);
    utp_stream_cleanup(&stream);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("peer reset only closes the read direction", "[stream][reset]")
{
    utp_packet_in_pool_t pool    = {};
    utp_stream_t         stream  = {};
    utp_frame_stream_t   decoded = {};
    utp_packet_in_t*     packet;

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
    REQUIRE(utp_stream_write_internal(&stream, reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);
    packet = stream_frame_packet(&pool, 0u, 0u, "xy", false, &decoded);
    REQUIRE(utp_stream_on_frame_packet(&stream, &decoded, packet) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_release(packet);
    REQUIRE(stream.send_buffer_length == 3u);
    REQUIRE(stream.recv_fragment_count == 1u);
    REQUIRE(stream.recv_pinned_memory_bytes != 0u);

    REQUIRE(utp_stream_on_peer_reset(&stream, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream.peer_reset);
    REQUIRE_FALSE(stream.local_fin_queued);
    REQUIRE_FALSE(stream.local_fin_sent);
    REQUIRE(stream.send_buffer_length == 3u);
    REQUIRE(stream.send_in_flight_bytes == 0u);
    REQUIRE(stream.recv_fragment_count == 0u);
    REQUIRE(stream.recv_pinned_memory_bytes == 0u);
    REQUIRE(utp_stream_write_internal(&stream, reinterpret_cast<const uint8_t*>("z"), 1u) == UTP_INTERNAL_ERROR_OK);

    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("connection allocates a local stream, sends STREAM frames, and the peer reads the data", "[stream]")
{
    const utp_address_t  active_address  = loopback_address(13001u);
    const utp_address_t  passive_address = loopback_address(13002u);
    utp_packet_in_pool_t receive_pool    = {};
    utp_connection_t     active          = {};
    utp_connection_t     passive         = {};
    uint32_t             stream_id       = UINT32_MAX;
    uint8_t              buffer[32]      = {};
    size_t               length          = 0u;
    bool                 fin             = false;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 11u, 22u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 22u, 11u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 4u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 0u);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_close_internal(utp_connection_find_stream_internal(&active, stream_id)) ==
            UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 100u, &receive_pool);
    REQUIRE(passive.recv_reassembly_fragment_count == 1u);
    REQUIRE(passive.recv_reassembly_memory_bytes >= 1280u);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(utp_stream_send_in_flight_bytes(stream) == 3u);
    }

    REQUIRE(utp_stream_read_internal(utp_connection_find_stream_internal(&passive, stream_id), buffer, sizeof(buffer),
                                     &length, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 3u);
    REQUIRE(std::memcmp(buffer, "abc", 3u) == 0);
    REQUIRE_FALSE(fin);
    REQUIRE(passive.recv_reassembly_fragment_count == 0u);
    REQUIRE(passive.recv_reassembly_memory_bytes == 0u);
    REQUIRE(utp_stream_read_internal(utp_connection_find_stream_internal(&passive, stream_id), buffer, sizeof(buffer),
                                     &length, &fin) == UTP_INTERNAL_ERROR_CLOSED);
    REQUIRE(length == 0u);
    REQUIRE(fin);
    REQUIRE(utp_connection_queue_ack(&passive, 200u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 200u, &receive_pool);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(utp_stream_send_in_flight_bytes(stream) == 0u);
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}

TEST_CASE("connection stream rejects write-side operations after connection close begins", "[stream][close]")
{
    const utp_address_t     peer       = loopback_address(13003u);
    utp_connection_t        connection = {};
    utp_stream_t*           stream;
    utp_stream_write_view_t view       = {};
    uint32_t                stream_id  = UINT32_MAX;
    size_t                  view_count = 0u;
    size_t                  capacity   = 0u;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 13u, 14u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    REQUIRE(utp_connection_create_stream_internal(&connection, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    stream = utp_connection_find_stream_internal(&connection, stream_id);
    REQUIRE(stream != nullptr);
    connection.state = UTP_CONNECTION_STATE_DRAINING;

    REQUIRE(utp_stream_write_internal(stream, reinterpret_cast<const uint8_t*>("data"), 4u) ==
            UTP_INTERNAL_ERROR_CLOSED);
    REQUIRE(utp_stream_close_internal(stream) == UTP_INTERNAL_ERROR_CLOSED);
    REQUIRE(utp_stream_acquire_write_views_internal(stream, &view, 1u, &view_count, &capacity) ==
            UTP_INTERNAL_ERROR_CLOSED);
    REQUIRE(utp_stream_commit_write_views_internal(stream, 1u) == UTP_INTERNAL_ERROR_CLOSED);

    utp_connection_cleanup(&connection);
}

TEST_CASE("connection piggybacks a pending ACK on one zero-copy STREAM packet", "[stream][ack]")
{
    const utp_address_t            active_address  = loopback_address(13031u);
    const utp_address_t            passive_address = loopback_address(13032u);
    const uint8_t                  ping            = UTP_FRAME_TYPE_PING;
    utp_connection_t               active          = {};
    utp_connection_t               passive         = {};
    std::array<uint8_t, 1280>      wire            = {};
    utp_packet_out_t*              packet;
    utp_packet_view_t              view = {};
    uint32_t                       stream_id;
    size_t                         length;
    size_t                         offset;
    uint8_t                        frame_type;
    const uint8_t*                 frame;
    size_t                         frame_length;
    std::array<uint8_t, 9u>        blocked       = {};
    const utp_frame_data_blocked_t blocked_frame = {UINT64_C(1234)};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 31u, 32u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 32u, 31u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&passive);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_packet_out_flatten(packet, wire.data(), wire.size(), &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_sent(&passive, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(&active, wire.data(), length, &passive_address, 100u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_ack_pending_count(&active) == 1u);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("data"), 4u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_close_internal(utp_connection_find_stream_internal(&active, stream_id)) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send_at(&active, 200u);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->frame_types == (UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) | UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)));
    REQUIRE(packet->transient_ack_size != 0u);
    REQUIRE(packet->slice_count == 2u);
    REQUIRE(packet->slices[1].source == UTP_PACKET_OUT_SLICE_EXTERNAL);
    REQUIRE(utp_connection_ack_pending_count(&active) == 1u);
    REQUIRE(utp_packet_out_flatten(packet, wire.data(), wire.size(), &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_view_decode(&view, wire.data(), length) == UTP_INTERNAL_ERROR_OK);
    offset = 0u;
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_ACK);
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_STREAM);
    REQUIRE(offset == view.payload_length);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_ack_pending_count(&active) == 0u);

    REQUIRE(utp_frame_data_blocked_encode(blocked.data(), blocked.size(), &blocked_frame) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_CTRL, blocked.data(), blocked.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&passive);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_packet_out_flatten(packet, wire.data(), wire.size(), &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_sent(&passive, packet, 300u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(&active, wire.data(), length, &passive_address, 300u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("next"), 4u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_close_internal(utp_connection_find_stream_internal(&active, stream_id)) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send_at(&active, 400u);
    REQUIRE(packet != nullptr);
    REQUIRE((packet->frame_types & (UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) | UTP_FRAME_BIT(UTP_FRAME_TYPE_MAX_DATA) |
                                    UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM))) ==
            (UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) | UTP_FRAME_BIT(UTP_FRAME_TYPE_MAX_DATA) |
             UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)));
    REQUIRE(utp_packet_out_flatten(packet, wire.data(), wire.size(), &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_view_decode(&view, wire.data(), length) == UTP_INTERNAL_ERROR_OK);
    offset = 0u;
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_ACK);
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_MAX_DATA);
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_STREAM);
    REQUIRE(offset == view.payload_length);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 400u) == UTP_INTERNAL_ERROR_OK);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection restores an unsent STREAM frame after a UDP write failure", "[stream][send]")
{
    const utp_address_t passive_address = loopback_address(13034u);
    utp_connection_t    active          = {};
    uint32_t            stream_id;
    utp_packet_out_t*   packet;
    utp_packet_out_t*   retry;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 33u, 34u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("data"), 4u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_close_internal(utp_connection_find_stream_internal(&active, stream_id)) ==
            UTP_INTERNAL_ERROR_OK);

    packet = utp_connection_next_packet_to_send_at(&active, 200u);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->stream_offset == 0u);
    REQUIRE(packet->stream_data_size == 4u);
    REQUIRE(utp_connection_find_stream_internal(&active, stream_id)->send_in_flight_bytes == 4u);
    utp_connection_on_packet_abandoned(&active, packet);
    utp_packet_out_pool_release(&active.packet_pool, packet);
    REQUIRE(utp_connection_find_stream_internal(&active, stream_id)->send_in_flight_bytes == 0u);
    REQUIRE(active.stream_data_sent_total == 0u);

    retry = utp_connection_next_packet_to_send_at(&active, 201u);
    REQUIRE(retry != nullptr);
    REQUIRE(retry->stream_offset == 0u);
    REQUIRE(retry->stream_data_size == 4u);
    utp_connection_on_packet_abandoned(&active, retry);
    utp_packet_out_pool_release(&active.packet_pool, retry);
    utp_connection_cleanup(&active);
}

TEST_CASE("strict stream scheduler honors priority and round-robins equal priorities", "[stream][scheduler]")
{
    const utp_address_t passive_address = loopback_address(13037u);
    utp_connection_t    connection      = {};
    uint32_t            first_id;
    uint32_t            second_id;
    utp_packet_out_t*   packet;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 37u, 38u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    REQUIRE(utp_connection_create_stream_internal(&connection, true, &first_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_create_stream_internal(&connection, true, &second_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&connection, first_id),
                                      reinterpret_cast<const uint8_t*>("first"), 5u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&connection, second_id),
                                      reinterpret_cast<const uint8_t*>("second"), 6u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_priority(utp_connection_find_stream_internal(&connection, first_id)) ==
            UTP_STREAM_PRIORITY_DEFAULT);
    REQUIRE(utp_stream_set_priority(utp_connection_find_stream_internal(&connection, first_id), 6u) == UTP_STATUS_OK);
    REQUIRE(utp_stream_set_priority(utp_connection_find_stream_internal(&connection, second_id), 1u) == UTP_STATUS_OK);
    REQUIRE(utp_stream_set_priority(utp_connection_find_stream_internal(&connection, second_id), 8u) ==
            UTP_STATUS_INVALID_ARGUMENT);

    packet = utp_connection_next_packet_to_send_at(&connection, 100u);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->stream_id == second_id);
    utp_connection_on_packet_abandoned(&connection, packet);
    utp_packet_out_pool_release(&connection.packet_pool, packet);

    REQUIRE(utp_stream_set_priority(utp_connection_find_stream_internal(&connection, first_id), 4u) == UTP_STATUS_OK);
    REQUIRE(utp_stream_set_priority(utp_connection_find_stream_internal(&connection, second_id), 4u) == UTP_STATUS_OK);
    connection.stream_scheduler_cursor = 0u;
    packet                             = utp_connection_next_packet_to_send_at(&connection, 101u);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->stream_id == first_id);
    utp_connection_on_packet_abandoned(&connection, packet);
    utp_packet_out_pool_release(&connection.packet_pool, packet);
    packet = utp_connection_next_packet_to_send_at(&connection, 102u);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->stream_id == second_id);
    utp_connection_on_packet_abandoned(&connection, packet);
    utp_packet_out_pool_release(&connection.packet_pool, packet);
    utp_connection_cleanup(&connection);
}

TEST_CASE("drr scheduler limits each STREAM fragment by its weighted deficit", "[stream][scheduler]")
{
    const utp_address_t        passive_address = loopback_address(13038u);
    std::array<uint8_t, 2048u> data            = {};
    utp_connection_t           connection      = {};
    uint32_t                   high_id;
    uint32_t                   low_id;
    utp_packet_out_t*          packet;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 38u, 39u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    REQUIRE(utp_connection_set_stream_scheduler_mode(&connection, UTP_STREAM_SCHEDULER_DRR) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_create_stream_internal(&connection, true, &high_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_create_stream_internal(&connection, true, &low_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_set_priority(utp_connection_find_stream_internal(&connection, high_id), 0u) == UTP_STATUS_OK);
    REQUIRE(utp_stream_set_priority(utp_connection_find_stream_internal(&connection, low_id), 7u) == UTP_STATUS_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&connection, high_id), data.data(),
                                      data.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&connection, low_id), data.data(),
                                      data.size()) == UTP_INTERNAL_ERROR_OK);

    packet = utp_connection_next_packet_to_send_at(&connection, 100u);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->stream_id == high_id);
    REQUIRE(packet->stream_data_size == 1244u);
    utp_connection_on_packet_abandoned(&connection, packet);
    utp_packet_out_pool_release(&connection.packet_pool, packet);
    packet = utp_connection_next_packet_to_send_at(&connection, 101u);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->stream_id == low_id);
    REQUIRE(packet->stream_data_size == 1200u);
    utp_connection_on_packet_abandoned(&connection, packet);
    utp_packet_out_pool_release(&connection.packet_pool, packet);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection stream reset sends RESET_STREAM and peer records reset", "[stream][reset]")
{
    const utp_address_t      active_address  = loopback_address(13035u);
    const utp_address_t      passive_address = loopback_address(13036u);
    utp_packet_in_pool_t     receive_pool    = {};
    utp_connection_t         active          = {};
    utp_connection_t         passive         = {};
    uint32_t                 stream_id       = UINT32_MAX;
    utp_packet_out_t*        packet;
    utp_packet_in_t*         wire = nullptr;
    size_t                   length;
    uint8_t                  frame_type;
    size_t                   frame_length;
    const uint8_t*           frame_data;
    utp_frame_reset_stream_t reset = {};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 111u, 112u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 112u, 111u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 1u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_reset_internal(utp_connection_find_stream_internal(&active, stream_id), UINT16_C(0x7788)) ==
            UTP_INTERNAL_ERROR_OK);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(stream->local_write_reset);
        REQUIRE(stream->send_buffer_length == 0u);
    }

    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    frame_data = first_packet_frame(packet, &frame_type, &frame_length);
    REQUIRE(frame_type == UTP_FRAME_TYPE_RESET_STREAM);
    REQUIRE(utp_frame_reset_stream_decode(&reset, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(reset.error_code == UINT16_C(0x7788));
    REQUIRE(reset.stream_id == stream_id);
    REQUIRE(reset.final_size == 0u);

    REQUIRE(utp_packet_in_pool_acquire(&receive_pool, &wire) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_flatten(packet, wire->data, wire->capacity, &length) == UTP_INTERNAL_ERROR_OK);
    wire->length = static_cast<uint16_t>(length);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_in_received(&passive, wire, &active_address, 100u) == UTP_INTERNAL_ERROR_OK);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&passive, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(stream->peer_reset);
    }

    utp_packet_in_release(wire);
    utp_packet_in_pool_cleanup(&receive_pool);
    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection commits RESET state before incoming stream callback", "[stream][reset][callback]")
{
    const utp_address_t     active_address  = loopback_address(13041u);
    const utp_address_t     passive_address = loopback_address(13042u);
    utp_packet_in_pool_t    receive_pool    = {};
    utp_connection_t        active          = {};
    utp_connection_t        passive         = {};
    incoming_terminal_probe probe           = {};
    uint32_t                stream_id       = UINT32_MAX;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 141u, 142u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 142u, 141u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 2u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);
    utp_connection_set_on_incoming_stream_internal(&active, on_incoming_terminal, &probe);

    REQUIRE(utp_connection_create_stream_internal(&passive, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_reset_internal(utp_connection_find_stream_internal(&passive, stream_id), UINT16_C(0x1234)) ==
            UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 100u, &receive_pool);

    REQUIRE(probe.incoming_count == 1);
    REQUIRE(probe.reset_visible);
    REQUIRE(probe.read_cancelled);
    REQUIRE(probe.closed_count == 1);

    utp_packet_in_pool_cleanup(&receive_pool);
    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection commits STOP_SENDING before incoming stream callback", "[stream][shutdown][callback]")
{
    const utp_address_t            active_address                       = loopback_address(13043u);
    const utp_address_t            passive_address                      = loopback_address(13044u);
    utp_packet_in_pool_t           receive_pool                         = {};
    utp_connection_t               active                               = {};
    utp_connection_t               passive                              = {};
    incoming_terminal_probe        probe                                = {};
    uint8_t                        payload[UTP_FRAME_STOP_SENDING_SIZE] = {};
    const utp_frame_stop_sending_t stop                                 = {UTP_PROTOCOL_STOP_SENDING_CANCELLED, 1u};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 151u, 152u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 152u, 151u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 2u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);
    utp_connection_set_on_incoming_stream_internal(&active, on_incoming_stopped, &probe);
    REQUIRE(utp_frame_stop_sending_encode(payload, sizeof(payload), &stop) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(transfer_control_payload(&passive, &active, &passive_address, payload, sizeof(payload), 100u,
                                     &receive_pool) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(probe.incoming_count == 1);
    REQUIRE(probe.write_cancelled);

    utp_packet_in_pool_cleanup(&receive_pool);
    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection defers writable notification until incoming callback returns", "[stream][callback]")
{
    const utp_address_t     active_address  = loopback_address(13052u);
    const utp_address_t     passive_address = loopback_address(13053u);
    utp_packet_in_pool_t    receive_pool    = {};
    utp_connection_t        active          = {};
    utp_connection_t        passive         = {};
    incoming_terminal_probe probe           = {};
    uint32_t                stream_id       = UINT32_MAX;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 211u, 212u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 212u, 211u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 2u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);
    utp_connection_set_on_incoming_stream_internal(&active, on_incoming_register_writable, &probe);

    REQUIRE(utp_connection_create_stream_internal(&passive, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&passive, stream_id),
                                      reinterpret_cast<const uint8_t*>("x"), 1u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 100u, &receive_pool);
    REQUIRE(probe.incoming_count == 1);
    REQUIRE(probe.writable_count == 1);
    REQUIRE_FALSE(probe.writable_during_incoming);

    utp_packet_in_pool_cleanup(&receive_pool);
    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection validates RESET final size and retires skipped receive bytes", "[stream][reset][flow]")
{
    const utp_address_t      active_address                       = loopback_address(13045u);
    const utp_address_t      passive_address                      = loopback_address(13046u);
    utp_packet_in_pool_t     receive_pool                         = {};
    utp_connection_t         active                               = {};
    utp_connection_t         passive                              = {};
    uint32_t                 stream_id                            = UINT32_MAX;
    uint8_t                  payload[UTP_FRAME_RESET_STREAM_SIZE] = {};
    utp_frame_reset_stream_t reset                                = {UINT16_C(0x2233), 0u, 2u};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 161u, 162u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 162u, 161u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 4u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&passive, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 1u);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&passive, stream_id),
                                      reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 100u, &receive_pool);
    reset.stream_id = stream_id;
    REQUIRE(utp_frame_reset_stream_encode(payload, sizeof(payload), &reset) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(transfer_control_payload(&passive, &active, &passive_address, payload, sizeof(payload), 200u,
                                     &receive_pool) == UTP_INTERNAL_ERROR_PROTOCOL);

    reset.final_size = 4u;
    REQUIRE(utp_frame_reset_stream_encode(payload, sizeof(payload), &reset) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(transfer_control_payload(&passive, &active, &passive_address, payload, sizeof(payload), 300u,
                                     &receive_pool) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(active.local_stream_data_received_total == 4u);
    REQUIRE(active.local_stream_data_consumed_total == 4u);
    REQUIRE(utp_connection_find_stream_internal(&active, stream_id)->peer_final_size == 4u);

    REQUIRE(transfer_control_payload(&passive, &active, &passive_address, payload, sizeof(payload), 400u,
                                     &receive_pool) == UTP_INTERNAL_ERROR_OK);
    reset.final_size = 5u;
    REQUIRE(utp_frame_reset_stream_encode(payload, sizeof(payload), &reset) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(transfer_control_payload(&passive, &active, &passive_address, payload, sizeof(payload), 500u,
                                     &receive_pool) == UTP_INTERNAL_ERROR_PROTOCOL);

    utp_packet_in_pool_cleanup(&receive_pool);
    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("stream shutdown read sends STOP_SENDING and peer replies RESET_STREAM", "[stream][shutdown]")
{
    const utp_address_t      active_address  = loopback_address(13039u);
    const utp_address_t      passive_address = loopback_address(13040u);
    utp_packet_in_pool_t     receive_pool    = {};
    utp_connection_t         active          = {};
    utp_connection_t         passive         = {};
    utp_stream_t*            stream;
    uint32_t                 stream_id = UINT32_MAX;
    utp_packet_out_t*        packet;
    utp_packet_in_t*         wire = nullptr;
    uint8_t                  frame_type;
    uint8_t                  read_buffer[1] = {};
    bool                     fin            = false;
    size_t                   read_length    = 0u;
    size_t                   frame_length;
    size_t                   wire_length;
    const uint8_t*           frame_data;
    utp_frame_stop_sending_t stop = {};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 131u, 132u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 132u, 131u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 2u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    stream = utp_connection_find_stream_internal(&active, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_shutdown_internal(stream, UTP_STREAM_SHUTDOWN_READ) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream->local_read_shutdown);
    REQUIRE(utp_stream_read_internal(stream, read_buffer, sizeof(read_buffer), &read_length, &fin) ==
            UTP_INTERNAL_ERROR_CLOSED);

    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    frame_data = first_packet_frame(packet, &frame_type, &frame_length);
    REQUIRE(frame_type == UTP_FRAME_TYPE_STOP_SENDING);
    REQUIRE(utp_frame_stop_sending_decode(&stop, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stop.stream_id == stream_id);
    REQUIRE(stop.error_code == UTP_PROTOCOL_STOP_SENDING_CANCELLED);
    REQUIRE(utp_packet_in_pool_acquire(&receive_pool, &wire) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_flatten(packet, wire->data, wire->capacity, &wire_length) == UTP_INTERNAL_ERROR_OK);
    wire->length = static_cast<uint16_t>(wire_length);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_in_received(&passive, wire, &active_address, 100u) == UTP_INTERNAL_ERROR_OK);
    utp_packet_in_release(wire);

    stream = utp_connection_find_stream_internal(&passive, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(stream->local_write_reset);
    REQUIRE(utp_stream_write_internal(stream, reinterpret_cast<const uint8_t*>("x"), 1u) ==
            UTP_INTERNAL_ERROR_CANCELLED);
    packet = utp_connection_next_packet_to_send(&passive);
    REQUIRE(packet != nullptr);
    (void)first_packet_frame(packet, &frame_type, &frame_length);
    REQUIRE(frame_type == UTP_FRAME_TYPE_RESET_STREAM);

    utp_packet_in_pool_cleanup(&receive_pool);
    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection does not retransmit old STREAM data after local stream reset", "[stream][reset]")
{
    const utp_address_t active_address  = loopback_address(13037u);
    const utp_address_t passive_address = loopback_address(13038u);
    utp_connection_t    active          = {};
    uint32_t            stream_id       = UINT32_MAX;
    utp_packet_out_t*   packet;
    uint8_t             frame_type;
    size_t              frame_length;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 121u, 122u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);

    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    (void)first_packet_frame(packet, &frame_type, &frame_length);
    REQUIRE(frame_type == UTP_FRAME_TYPE_STREAM);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_stream_reset_internal(utp_connection_find_stream_internal(&active, stream_id), UINT16_C(0x0102)) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    (void)first_packet_frame(packet, &frame_type, &frame_length);
    REQUIRE(frame_type == UTP_FRAME_TYPE_RESET_STREAM);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 110u) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_connection_on_retransmission_timeout(&active, 200u) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    (void)first_packet_frame(packet, &frame_type, &frame_length);
    REQUIRE(frame_type == UTP_FRAME_TYPE_RESET_STREAM);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 210u) == UTP_INTERNAL_ERROR_OK);

    utp_connection_cleanup(&active);
    (void)active_address;
}

TEST_CASE("connection drops scheduled STREAM bytes when reset precedes transmission", "[stream][reset][flow]")
{
    const utp_address_t      passive_address = loopback_address(13047u);
    utp_connection_t         active          = {};
    utp_stream_t*            stream;
    utp_packet_out_t*        packet;
    uint32_t                 stream_id = UINT32_MAX;
    uint8_t                  frame_type;
    size_t                   frame_length;
    const uint8_t*           frame_data;
    utp_frame_reset_stream_t reset = {};
    const uint8_t            ping  = UTP_FRAME_TYPE_PING;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 171u, 172u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send_at(&active, 50u);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 50u) == UTP_INTERNAL_ERROR_OK);
    active.bbr_congestion.cwnd = 1u;

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    stream = utp_connection_find_stream_internal(&active, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write_internal(stream, reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_next_packet_to_send_at(&active, 100u) == nullptr);
    REQUIRE(utp_send_control_scheduled_packet_count(&active.send_control) == 1u);
    REQUIRE(active.stream_data_sent_total == 3u);
    REQUIRE(stream->local_max_stream_offset_sent == 0u);

    REQUIRE(utp_stream_reset_internal(stream, UINT16_C(0x3344)) == UTP_INTERNAL_ERROR_OK);
    active.bbr_congestion.cwnd = UINT64_MAX;
    packet                     = utp_connection_next_packet_to_send_at(&active, 200u);
    REQUIRE(packet != nullptr);
    frame_data = first_packet_frame(packet, &frame_type, &frame_length);
    REQUIRE(frame_type == UTP_FRAME_TYPE_RESET_STREAM);
    REQUIRE(utp_frame_reset_stream_decode(&reset, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(reset.final_size == 0u);
    REQUIRE(active.stream_data_sent_total == 0u);

    REQUIRE(utp_connection_on_packet_sent(&active, packet, 200u) == UTP_INTERNAL_ERROR_OK);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection reset final size tracks bytes actually transmitted", "[stream][reset]")
{
    const utp_address_t      passive_address = loopback_address(13048u);
    utp_connection_t         active          = {};
    utp_stream_t*            stream;
    utp_packet_out_t*        packet;
    uint32_t                 stream_id = UINT32_MAX;
    uint8_t                  frame_type;
    size_t                   frame_length;
    const uint8_t*           frame_data;
    utp_frame_reset_stream_t reset = {};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 181u, 182u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    stream = utp_connection_find_stream_internal(&active, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write_internal(stream, reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream->local_max_stream_offset_sent == 3u);

    REQUIRE(utp_stream_reset_internal(stream, UINT16_C(0x4455)) == UTP_INTERNAL_ERROR_OK);
    packet     = utp_connection_next_packet_to_send(&active);
    frame_data = first_packet_frame(packet, &frame_type, &frame_length);
    REQUIRE(frame_type == UTP_FRAME_TYPE_RESET_STREAM);
    REQUIRE(utp_frame_reset_stream_decode(&reset, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(reset.final_size == 3u);

    REQUIRE(utp_connection_on_packet_sent(&active, packet, 200u) == UTP_INTERNAL_ERROR_OK);
    utp_connection_cleanup(&active);
}

TEST_CASE("shutdown read retires buffered and later in-flight bytes", "[stream][shutdown][flow]")
{
    const utp_address_t  active_address  = loopback_address(13049u);
    const utp_address_t  passive_address = loopback_address(13050u);
    utp_packet_in_pool_t receive_pool    = {};
    utp_connection_t     active          = {};
    utp_connection_t     passive         = {};
    utp_stream_t*        stream;
    uint32_t             stream_id = UINT32_MAX;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 191u, 192u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 192u, 191u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 4u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 100u, &receive_pool);
    stream = utp_connection_find_stream_internal(&passive, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(stream->recv_buffered_bytes == 3u);
    REQUIRE(utp_stream_shutdown_internal(stream, UTP_STREAM_SHUTDOWN_READ) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream->recv_buffered_bytes == 0u);
    REQUIRE(passive.local_stream_data_received_total == 3u);
    REQUIRE(passive.local_stream_data_consumed_total == 3u);

    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("de"), 2u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 200u, &receive_pool);
    REQUIRE(stream->recv_buffered_bytes == 0u);
    REQUIRE(passive.local_stream_data_received_total == 5u);
    REQUIRE(passive.local_stream_data_consumed_total == 5u);

    utp_packet_in_pool_cleanup(&receive_pool);
    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection stream terminal table evicts and reuses its oldest slot", "[stream][terminal]")
{
    const utp_address_t passive_address = loopback_address(13051u);
    utp_connection_t    connection      = {};
    utp_stream_t*       stream;
    uint32_t            stream_id = UINT32_MAX;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 201u, 202u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_set_stream_terminal_capacity(&connection, 2u) == UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);

    for (uint32_t index = 0u; index < 4u; ++index) {
        REQUIRE(utp_connection_create_stream_internal(&connection, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
        stream = utp_connection_find_stream_internal(&connection, stream_id);
        REQUIRE(stream != nullptr);
        stream->local_write_reset = true;
        stream->peer_reset        = true;
    }
    REQUIRE(connection.stream_terminal_count == 2u);
    REQUIRE(connection.stream_terminal_allocated == 2u);
    REQUIRE(utp_hash_table_count(&connection.stream_terminals) == 2u);
    REQUIRE(connection.stream_terminal_oldest != nullptr);
    REQUIRE(connection.stream_terminal_newest != nullptr);
    REQUIRE(connection.stream_terminal_oldest->stream_id == 4u);
    REQUIRE(connection.stream_terminal_newest->stream_id == 8u);

    utp_connection_cleanup(&connection);
}

TEST_CASE("connection stream terminal table grows from small blocks", "[stream][terminal]")
{
    const utp_address_t passive_address = loopback_address(13052u);
    utp_connection_t    connection      = {};
    utp_stream_t*       stream;
    uint32_t            stream_id = UINT32_MAX;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 203u, 204u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    REQUIRE(connection.stream_terminal_allocated == 0u);

    for (uint32_t index = 0u; index < 9u; ++index) {
        REQUIRE(utp_connection_create_stream_internal(&connection, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
        stream = utp_connection_find_stream_internal(&connection, stream_id);
        REQUIRE(stream != nullptr);
        stream->local_write_reset = true;
        stream->peer_reset        = true;
    }
    REQUIRE(connection.stream_terminal_count == 8u);
    REQUIRE(connection.stream_terminal_allocated == 8u);

    REQUIRE(utp_connection_create_stream_internal(&connection, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    stream = utp_connection_find_stream_internal(&connection, stream_id);
    REQUIRE(stream != nullptr);
    stream->local_write_reset = true;
    stream->peer_reset        = true;
    REQUIRE(connection.stream_terminal_count == 9u);
    REQUIRE(connection.stream_terminal_allocated == 16u);

    utp_connection_cleanup(&connection);
}

TEST_CASE("connection STREAM packets use an external data slice", "[stream][zero-copy]")
{
    const utp_address_t active_address  = loopback_address(13011u);
    const utp_address_t passive_address = loopback_address(13012u);
    utp_connection_t    active          = {};
    uint32_t            stream_id       = UINT32_MAX;
    utp_packet_out_t*   packet;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 31u, 32u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("abcd"), 4u) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->slice_count == 2u);
    REQUIRE(packet->slices[0].source == UTP_PACKET_OUT_SLICE_RAW_OFFSET);
    REQUIRE(packet->slices[0].length == UTP_PACKET_HEADER_SIZE + UTP_FRAME_STREAM_HEADER_SIZE);
    REQUIRE(packet->slices[1].source == UTP_PACKET_OUT_SLICE_EXTERNAL);
    REQUIRE(packet->slices[1].length == 4u);
    REQUIRE(packet->slices[1].data != nullptr);

    utp_packet_out_pool_release(&active.packet_pool, packet);
    utp_connection_cleanup(&active);
    (void)active_address;
}

TEST_CASE("connection applies incoming flow-control limit updates monotonically", "[stream][flow]")
{
    const utp_address_t               active_address  = loopback_address(13003u);
    const utp_address_t               passive_address = loopback_address(13004u);
    utp_packet_in_pool_t              receive_pool    = {};
    utp_connection_t                  active          = {};
    utp_connection_t                  passive         = {};
    uint32_t                          stream_id       = UINT32_MAX;
    uint8_t                           payload[13]     = {};
    const utp_frame_max_data_t        max_data        = {UINT64_C(123456)};
    const utp_frame_max_stream_data_t max_stream_data = {0u, UINT64_C(654321)};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 11u, 22u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 22u, 11u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 2u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 0u);
    active.peer_max_data = 1u;
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&active, stream_id);

        REQUIRE(stream != nullptr);
        stream->peer_max_stream_data = 1u;
    }

    REQUIRE(utp_frame_max_data_encode(payload, sizeof(payload), &max_data) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_CTRL, payload, 9u, false) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 100u, &receive_pool);
    REQUIRE(active.peer_max_data == max_data.maximum_data);

    REQUIRE(utp_frame_max_stream_data_encode(payload, sizeof(payload), &max_stream_data) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), false) ==
            UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 200u, &receive_pool);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(stream->peer_max_stream_data == max_stream_data.maximum_stream_data);
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}

TEST_CASE("connection applies MAX_STREAM_DATA received before a local stream exists", "[stream][flow]")
{
    const utp_address_t               active_address  = loopback_address(13031u);
    const utp_address_t               passive_address = loopback_address(13032u);
    utp_packet_in_pool_t              receive_pool    = {};
    utp_connection_t                  active          = {};
    utp_connection_t                  passive         = {};
    uint32_t                          stream_id       = UINT32_MAX;
    uint8_t                           payload[13]     = {};
    const utp_frame_max_stream_data_t max_stream_data = {0u, UINT64_C(12345678)};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 91u, 92u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 92u, 91u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 1u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_frame_max_stream_data_encode(payload, sizeof(payload), &max_stream_data) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), false) ==
            UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 100u, &receive_pool);
    REQUIRE(utp_hash_table_count(&active.pending_peer_max_stream_data) == 1u);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == max_stream_data.stream_id);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(stream->peer_max_stream_data == max_stream_data.maximum_stream_data);
    }
    REQUIRE(utp_hash_table_count(&active.pending_peer_max_stream_data) == 0u);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}

TEST_CASE("connection receive reassembly accounting rejects connection-level fragment exhaustion", "[stream][flow]")
{
    const utp_address_t  active_address  = loopback_address(13033u);
    const utp_address_t  passive_address = loopback_address(13034u);
    utp_packet_in_pool_t receive_pool    = {};
    utp_connection_t     active          = {};
    utp_connection_t     passive         = {};
    uint32_t             stream_id       = UINT32_MAX;
    utp_packet_out_t*    packet;
    utp_packet_in_t*     wire = nullptr;
    size_t               length;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 101u, 102u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 102u, 101u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 1u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    passive.recv_reassembly_fragment_count = UTP_CONNECTION_RECV_REASSEMBLY_FRAGMENT_LIMIT;

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("x"), 1u) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_packet_in_pool_acquire(&receive_pool, &wire) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_flatten(packet, wire->data, wire->capacity, &length) == UTP_INTERNAL_ERROR_OK);
    wire->length = static_cast<uint16_t>(length);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_in_received(&passive, wire, &active_address, 100u) ==
            UTP_INTERNAL_ERROR_WOULD_BLOCK);
    REQUIRE(passive.recv_reassembly_fragment_count == UTP_CONNECTION_RECV_REASSEMBLY_FRAGMENT_LIMIT);
    REQUIRE(passive.recv_reassembly_memory_bytes == 0u);
    REQUIRE(utp_connection_find_stream_internal(&passive, stream_id) == nullptr);

    passive.recv_reassembly_fragment_count = 0u;
    REQUIRE(utp_connection_on_packet_in_received(&passive, wire, &active_address, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_find_stream_internal(&passive, stream_id) != nullptr);

    utp_packet_in_release(wire);
    utp_packet_in_pool_cleanup(&receive_pool);
    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection send path respects peer MAX_DATA", "[stream][flow]")
{
    const utp_address_t active_address  = loopback_address(13021u);
    const utp_address_t passive_address = loopback_address(13022u);
    utp_connection_t    active          = {};
    uint32_t            stream_id       = UINT32_MAX;
    utp_packet_out_t*   packet;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 41u, 42u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    active.peer_max_data = 2u;

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("abcde"), 5u) == UTP_INTERNAL_ERROR_OK);

    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->stream_data_size == 2u);
    REQUIRE(packet->slices[1].length == 2u);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(active.stream_data_sent_total == 2u);
    packet = utp_connection_next_packet_to_send_at(&active, 200u);
    REQUIRE(packet != nullptr);
    {
        uint8_t        frame_type;
        size_t         frame_length;
        const uint8_t* frame_data = first_packet_frame(packet, &frame_type, &frame_length);

        REQUIRE(frame_type == UTP_FRAME_TYPE_DATA_BLOCKED);
        REQUIRE(frame_length == 9u);
        (void)frame_data;
    }
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 200u) == UTP_INTERNAL_ERROR_OK);

    utp_connection_cleanup(&active);
    (void)active_address;
}

TEST_CASE("connection responds to DataBlocked and StreamDataBlocked with current limits", "[stream][flow]")
{
    const utp_address_t             active_address  = loopback_address(13027u);
    const utp_address_t             passive_address = loopback_address(13028u);
    utp_packet_in_pool_t            receive_pool    = {};
    utp_connection_t                active          = {};
    utp_connection_t                passive         = {};
    uint32_t                        stream_id       = UINT32_MAX;
    uint8_t                         payload[13]     = {};
    const utp_frame_data_blocked_t  data_blocked    = {0u};
    utp_frame_stream_data_blocked_t stream_blocked  = {0u, 0u};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 71u, 72u, &passive_address, 16u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 72u, 71u, &active_address, 16u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 4u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("x"), 1u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 100u, &receive_pool);

    REQUIRE(utp_frame_data_blocked_encode(payload, sizeof(payload), &data_blocked) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_CTRL, payload, 9u, false) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 200u, &receive_pool);
    {
        uint8_t              frame_type;
        size_t               frame_length;
        const uint8_t*       frame_data;
        utp_frame_max_data_t max_data = {};
        utp_packet_out_t*    packet   = utp_connection_next_packet_to_send(&passive);

        frame_data = first_packet_frame(packet, &frame_type, &frame_length);
        REQUIRE(frame_type == UTP_FRAME_TYPE_MAX_DATA);
        REQUIRE(utp_frame_max_data_decode(&max_data, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(max_data.maximum_data == passive.local_max_data_advertised);
        REQUIRE(utp_connection_on_packet_sent(&passive, packet, 201u) == UTP_INTERNAL_ERROR_OK);
    }

    stream_blocked.stream_id = stream_id;
    REQUIRE(utp_frame_stream_data_blocked_encode(payload, sizeof(payload), &stream_blocked) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_CTRL, payload, 13u, false) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 300u, &receive_pool);
    {
        uint8_t                     frame_type;
        size_t                      frame_length;
        const uint8_t*              frame_data;
        utp_frame_max_stream_data_t max_stream_data = {};
        utp_packet_out_t*           packet          = utp_connection_next_packet_to_send(&passive);

        frame_data = first_packet_frame(packet, &frame_type, &frame_length);
        REQUIRE(frame_type == UTP_FRAME_TYPE_MAX_STREAM_DATA);
        REQUIRE(utp_frame_max_stream_data_decode(&max_stream_data, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(max_stream_data.stream_id == stream_id);
        REQUIRE(max_stream_data.maximum_stream_data == UTP_STREAM_DEFAULT_FLOW_WINDOW);
        REQUIRE(utp_connection_on_packet_sent(&passive, packet, 301u) == UTP_INTERNAL_ERROR_OK);
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}

TEST_CASE("connection rejects STREAM data beyond local connection receive window", "[stream][flow]")
{
    const utp_address_t  active_address  = loopback_address(13023u);
    const utp_address_t  passive_address = loopback_address(13024u);
    utp_packet_in_pool_t receive_pool    = {};
    utp_connection_t     active          = {};
    utp_connection_t     passive         = {};
    uint32_t             stream_id       = UINT32_MAX;
    utp_packet_out_t*    packet;
    utp_packet_in_t*     wire = nullptr;
    size_t               length;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 51u, 52u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 52u, 51u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 1u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    passive.local_max_data_advertised = 2u;

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("abc"), 3u) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_packet_in_pool_acquire(&receive_pool, &wire) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_flatten(packet, wire->data, wire->capacity, &length) == UTP_INTERNAL_ERROR_OK);
    wire->length = static_cast<uint16_t>(length);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_in_received(&passive, wire, &active_address, 100u) ==
            UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL);
    REQUIRE(utp_internal_error_to_status(UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL) == UTP_STATUS_STREAM_FLOW_CONTROL);
    REQUIRE(passive.local_stream_data_received_total == 0u);

    utp_packet_in_release(wire);
    utp_packet_in_pool_cleanup(&receive_pool);
    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection sends MAX_DATA and MAX_STREAM_DATA after application consumes stream bytes", "[stream][flow]")
{
    const uint64_t       connection_window = (uint64_t)UTP_STREAM_DEFAULT_FLOW_WINDOW * 4u;
    const uint64_t       stream_window     = UTP_STREAM_DEFAULT_FLOW_WINDOW;
    const utp_address_t  active_address    = loopback_address(13025u);
    const utp_address_t  passive_address   = loopback_address(13026u);
    utp_packet_in_pool_t receive_pool      = {};
    utp_connection_t     active            = {};
    utp_connection_t     passive           = {};
    uint32_t             stream_id         = UINT32_MAX;
    uint8_t              buffer[16]        = {};
    size_t               length            = 0u;
    bool                 fin               = false;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 61u, 62u, &passive_address, 16u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 62u, 61u, &active_address, 16u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 4u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("abcdefghij"), 10u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 100u, &receive_pool);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&passive, stream_id);

        REQUIRE(stream != nullptr);
        passive.local_max_data_advertised        = connection_window - (connection_window / 10u);
        stream->local_max_stream_data_advertised = stream_window - (stream_window / 10u);
    }

    REQUIRE(utp_stream_read_internal(utp_connection_find_stream_internal(&passive, stream_id), buffer, sizeof(buffer),
                                     &length, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 10u);
    REQUIRE(passive.local_stream_data_consumed_total == 10u);

    {
        uint8_t                     first_frame_type;
        uint8_t                     second_frame_type;
        size_t                      first_frame_length;
        size_t                      second_frame_length;
        size_t                      offset = 0u;
        const uint8_t*              first_frame_data;
        const uint8_t*              second_frame_data;
        utp_frame_max_data_t        max_data        = {};
        utp_frame_max_stream_data_t max_stream_data = {};
        utp_packet_out_t* const     packet          = utp_connection_next_packet_to_send(&passive);

        REQUIRE(packet != nullptr);
        {
            utp_packet_view_t view = {};

            REQUIRE(utp_packet_view_decode(&view, packet->raw_data, packet->data_size) == UTP_INTERNAL_ERROR_OK);
            REQUIRE(utp_packet_view_next_frame(&view, &offset, &first_frame_type, &first_frame_data,
                                               &first_frame_length) == UTP_INTERNAL_ERROR_OK);
            REQUIRE(utp_packet_view_next_frame(&view, &offset, &second_frame_type, &second_frame_data,
                                               &second_frame_length) == UTP_INTERNAL_ERROR_OK);
            REQUIRE(offset == view.payload_length);
        }
        REQUIRE(first_frame_type == UTP_FRAME_TYPE_MAX_DATA);
        REQUIRE(second_frame_type == UTP_FRAME_TYPE_MAX_STREAM_DATA);
        REQUIRE(utp_frame_max_data_decode(&max_data, first_frame_data, first_frame_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(max_data.maximum_data == connection_window + 10u);
        REQUIRE(utp_frame_max_stream_data_decode(&max_stream_data, second_frame_data, second_frame_length) ==
                UTP_INTERNAL_ERROR_OK);
        REQUIRE(max_stream_data.stream_id == stream_id);
        REQUIRE(max_stream_data.maximum_stream_data == stream_window + 10u);
        REQUIRE(passive.local_max_data_advertised == connection_window - (connection_window / 10u));
        REQUIRE(utp_connection_on_packet_sent(&passive, packet, 200u) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(passive.local_max_data_advertised == connection_window + 10u);
        REQUIRE(utp_send_control_unacked_packet_count(&passive.send_control) == 1u);
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}

TEST_CASE("connection read-view commit advances flow-control windows", "[stream][read_view][flow]")
{
    const uint64_t         connection_window = (uint64_t)UTP_STREAM_DEFAULT_FLOW_WINDOW * 4u;
    const utp_address_t    active_address    = loopback_address(13029u);
    const utp_address_t    passive_address   = loopback_address(13030u);
    utp_packet_in_pool_t   receive_pool      = {};
    utp_connection_t       active            = {};
    utp_connection_t       passive           = {};
    uint32_t               stream_id         = UINT32_MAX;
    utp_stream_read_view_t view              = {};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 81u, 82u, &passive_address, 16u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 82u, 81u, &active_address, 16u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 4u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_write_internal(utp_connection_find_stream_internal(&active, stream_id),
                                      reinterpret_cast<const uint8_t*>("abcd"), 4u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 100u, &receive_pool);
    passive.local_max_data_advertised = connection_window - (connection_window / 10u);

    REQUIRE(utp_stream_acquire_read_view_internal(utp_connection_find_stream_internal(&passive, stream_id), &view) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.length == 4u);
    REQUIRE(utp_stream_commit_read_view_internal(utp_connection_find_stream_internal(&passive, stream_id), view.offset,
                                                 view.length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(passive.local_stream_data_consumed_total == 4u);
    {
        uint8_t              frame_type;
        size_t               frame_length;
        const uint8_t*       frame_data;
        utp_frame_max_data_t max_data = {};
        utp_packet_out_t*    packet   = utp_connection_next_packet_to_send(&passive);

        frame_data = first_packet_frame(packet, &frame_type, &frame_length);
        REQUIRE(frame_type == UTP_FRAME_TYPE_MAX_DATA);
        REQUIRE(utp_frame_max_data_decode(&max_data, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(max_data.maximum_data == connection_window + 4u);
        REQUIRE(utp_connection_on_packet_sent(&passive, packet, 200u) == UTP_INTERNAL_ERROR_OK);
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}

TEST_CASE("stream creation waits for MAX_STREAMS after sending STREAMS_BLOCKED", "[stream][stream_limit]")
{
    const utp_address_t       active_address  = loopback_address(13035u);
    const utp_address_t       passive_address = loopback_address(13036u);
    utp_packet_in_pool_t      receive_pool    = {};
    utp_connection_t          active          = {};
    utp_connection_t          passive         = {};
    uint32_t                  stream_id       = UINT32_MAX;
    uint8_t                   frame_type      = UTP_FRAME_TYPE_INVALID;
    size_t                    frame_length    = 0u;
    utp_frame_streams_limit_t blocked         = {};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 101u, 102u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 102u, 101u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 4u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    active.peer_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL] = 1u;

    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 0u);
    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_STREAM_LIMIT);
    {
        utp_packet_out_t* packet     = utp_connection_next_packet_to_send_at(&active, 100u);
        const uint8_t*    frame_data = first_packet_frame(packet, &frame_type, &frame_length);

        REQUIRE(frame_type == UTP_FRAME_TYPE_STREAMS_BLOCKED);
        REQUIRE(utp_frame_streams_blocked_decode(&blocked, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(blocked.stream_type == UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL);
        REQUIRE(blocked.stream_limit == 1u);
        REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_connection_on_packet_received(&passive, packet->raw_data, packet->data_size, &active_address,
                                                  100u) == UTP_INTERNAL_ERROR_OK);
    }
    {
        utp_packet_out_t* packet = utp_connection_next_packet_to_send_at(&passive, 200u);

        REQUIRE(packet != nullptr);
        REQUIRE(utp_connection_on_packet_sent(&passive, packet, 200u) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_connection_on_packet_received(&active, packet->raw_data, packet->data_size, &passive_address,
                                                  200u) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(active.peer_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL] ==
            passive.local_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL]);
    REQUIRE(utp_connection_create_stream_internal(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 4u);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}

TEST_CASE("connection supports the default bidirectional and unidirectional stream quotas", "[stream][stream_limit]")
{
    const utp_address_t peer       = loopback_address(13037u);
    utp_connection_t    connection = {};
    uint32_t            stream_id  = UINT32_MAX;
    uint32_t            index;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 103u, 104u, &peer, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);

    for (index = 0u; index < 64u; ++index) {
        REQUIRE(utp_connection_create_stream_internal(&connection, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(stream_id == index * UTP_STREAM_TYPES);
    }
    REQUIRE(utp_connection_create_stream_internal(&connection, true, &stream_id) == UTP_INTERNAL_ERROR_STREAM_LIMIT);

    for (index = 0u; index < 32u; ++index) {
        REQUIRE(utp_connection_create_stream_internal(&connection, false, &stream_id) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(stream_id == index * UTP_STREAM_TYPES + UTP_STREAM_UNIDIRECTIONAL);
    }
    REQUIRE(utp_connection_create_stream_internal(&connection, false, &stream_id) == UTP_INTERNAL_ERROR_STREAM_LIMIT);

    utp_connection_cleanup(&connection);
}

TEST_CASE("connection stream table has no fixed local concurrency cap", "[stream][stream_limit]")
{
    const utp_address_t peer       = loopback_address(13042u);
    utp_connection_t    connection = {};
    uint32_t            stream_id  = UINT32_MAX;
    uint32_t            index;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 109u, 110u, &peer, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    connection.peer_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL] = 97u;

    for (index = 0u; index < 97u; ++index) {
        REQUIRE(utp_connection_create_stream_internal(&connection, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(stream_id == index * UTP_STREAM_TYPES);
    }
    REQUIRE(utp_hash_table_count(&connection.streams) == 97u);

    utp_connection_cleanup(&connection);
}

TEST_CASE("unidirectional streams enforce their sender and receiver roles", "[stream][direction]")
{
    const utp_address_t  active_address  = loopback_address(13043u);
    const utp_address_t  passive_address = loopback_address(13044u);
    utp_packet_in_pool_t receive_pool    = {};
    utp_connection_t     active          = {};
    utp_connection_t     passive         = {};
    uint8_t              received[8]     = {};
    const uint8_t        data[]          = {'u', 'n', 'i'};
    uint32_t             local_stream_id;
    uint32_t             peer_stream_id;
    size_t               received_length;
    bool                 fin;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 111u, 112u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 112u, 111u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 2u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);

    REQUIRE(utp_connection_create_stream_internal(&active, false, &local_stream_id) == UTP_INTERNAL_ERROR_OK);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&active, local_stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(utp_stream_local_can_send(stream));
        REQUIRE_FALSE(utp_stream_local_can_receive(stream));
        REQUIRE(utp_stream_read_internal(stream, received, sizeof(received), &received_length, &fin) ==
                UTP_INTERNAL_ERROR_STATE);
    }

    REQUIRE(utp_connection_create_stream_internal(&passive, false, &peer_stream_id) == UTP_INTERNAL_ERROR_OK);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&passive, peer_stream_id);

        REQUIRE(utp_stream_write_internal(stream, data, sizeof(data)) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_stream_close_internal(stream) == UTP_INTERNAL_ERROR_OK);
    }
    transfer_next_packet(&passive, &active, &passive_address, 100u, &receive_pool);
    {
        utp_stream_t* stream = utp_connection_find_stream_internal(&active, peer_stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE_FALSE(utp_stream_local_can_send(stream));
        REQUIRE(utp_stream_local_can_receive(stream));
        REQUIRE(utp_stream_write_internal(stream, data, sizeof(data)) == UTP_INTERNAL_ERROR_STATE);
        REQUIRE(utp_stream_close_internal(stream) == UTP_INTERNAL_ERROR_STATE);
        REQUIRE(utp_stream_reset_internal(stream, 7u) == UTP_INTERNAL_ERROR_STATE);
        REQUIRE(utp_stream_read_internal(stream, received, sizeof(received), &received_length, &fin) ==
                UTP_INTERNAL_ERROR_OK);
        REQUIRE(received_length == sizeof(data));
        REQUIRE(std::memcmp(received, data, sizeof(data)) == 0);
        REQUIRE(utp_stream_read_internal(stream, received, sizeof(received), &received_length, &fin) ==
                UTP_INTERNAL_ERROR_CLOSED);
        REQUIRE(utp_stream_is_closed(stream));
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}

TEST_CASE("peer STREAM on a locally initiated unidirectional stream is a protocol error", "[stream][direction]")
{
    const utp_address_t      active_address  = loopback_address(13045u);
    const utp_address_t      passive_address = loopback_address(13046u);
    utp_connection_t         active          = {};
    utp_connection_t         passive         = {};
    uint32_t                 stream_id;
    const uint8_t            data[]                                               = {'x'};
    uint8_t                  payload[UTP_FRAME_STREAM_HEADER_SIZE + sizeof(data)] = {};
    const utp_frame_stream_t frame      = {UTP_STREAM_FLAG_NONE, 0u, 0u, data, (uint16_t)sizeof(data)};
    uint8_t                  wire[1280] = {};
    utp_packet_out_t*        packet;
    size_t                   wire_length;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 113u, 114u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 114u, 113u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);
    REQUIRE(utp_connection_create_stream_internal(&active, false, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 2u);
    {
        utp_frame_stream_t invalid = frame;

        invalid.stream_id = stream_id;
        REQUIRE(utp_frame_stream_encode(payload, sizeof(payload), &invalid) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), false) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&passive);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_packet_out_flatten(packet, wire, sizeof(wire), &wire_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_sent(&passive, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(&active, wire, wire_length, &passive_address, 100u) ==
            UTP_INTERNAL_ERROR_PROTOCOL);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("stream reclamation retains a stream referenced by an unacked FIN packet", "[stream][lifetime]")
{
    const utp_address_t peer       = loopback_address(13047u);
    utp_connection_t    connection = {};
    uint32_t            first_id;
    uint32_t            second_id;
    utp_stream_t*       first;
    utp_packet_out_t*   packet;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 115u, 116u, &peer, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    REQUIRE(utp_connection_create_stream_internal(&connection, true, &first_id) == UTP_INTERNAL_ERROR_OK);
    first           = utp_connection_find_stream_internal(&connection, first_id);
    first->peer_fin = true;
    REQUIRE(utp_stream_close_internal(first) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send_at(&connection, 100u);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->stream_id == first_id);
    REQUIRE(packet->stream_data_size == 0u);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, 100u) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_connection_create_stream_internal(&connection, true, &second_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(second_id != first_id);
    REQUIRE(utp_connection_find_stream_internal(&connection, first_id) == first);

    utp_connection_cleanup(&connection);
}

TEST_CASE("a completed peer stream releases a MAX_STREAMS credit", "[stream][stream_limit]")
{
    const utp_address_t       active_address  = loopback_address(13038u);
    const utp_address_t       passive_address = loopback_address(13039u);
    utp_packet_in_pool_t      receive_pool    = {};
    utp_connection_t          active          = {};
    utp_connection_t          passive         = {};
    utp_stream_t*             stream          = nullptr;
    utp_packet_out_t*         packet          = nullptr;
    utp_frame_streams_limit_t maximum         = {};
    const uint8_t*            frame           = nullptr;
    size_t                    frame_length    = 0u;
    uint32_t                  stream_id       = UINT32_MAX;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 105u, 106u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 106u, 105u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 4u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);
    active.local_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL] = 1u;

    REQUIRE(utp_connection_create_stream_internal(&passive, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 1u);
    stream = utp_connection_find_stream_internal(&passive, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_close_internal(stream) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 100u, &receive_pool);

    stream = utp_connection_find_stream_internal(&active, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(stream->peer_fin);
    REQUIRE(utp_stream_close_internal(stream) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 200u, &receive_pool);

    packet = utp_connection_next_packet_to_send_at(&active, 300u);
    REQUIRE(packet != nullptr);
    frame = packet_frame_of_type(packet, UTP_FRAME_TYPE_MAX_STREAMS, &frame_length);
    REQUIRE(frame != nullptr);
    REQUIRE(utp_frame_max_streams_decode(&maximum, frame, frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(maximum.stream_type == UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL);
    REQUIRE(maximum.stream_limit == 2u);
    REQUIRE(active.local_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL] == 2u);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 300u) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_connection_create_stream_internal(&passive, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 5u);
    stream = utp_connection_find_stream_internal(&passive, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_close_internal(stream) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 400u, &receive_pool);
    REQUIRE(utp_connection_find_stream_internal(&active, stream_id) != nullptr);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}

TEST_CASE("peer streams beyond MAX_STREAMS are rejected", "[stream][stream_limit]")
{
    const utp_address_t  active_address  = loopback_address(13040u);
    const utp_address_t  passive_address = loopback_address(13041u);
    utp_packet_in_pool_t receive_pool    = {};
    utp_connection_t     active          = {};
    utp_connection_t     passive         = {};
    utp_stream_t*        stream          = nullptr;
    utp_packet_out_t*    packet          = nullptr;
    utp_packet_in_t*     wire            = nullptr;
    uint32_t             stream_id       = UINT32_MAX;
    size_t               length          = 0u;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 107u, 108u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 108u, 107u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&receive_pool, nullptr, 2u, 1280u) == UTP_INTERNAL_ERROR_OK);
    active.state  = UTP_CONNECTION_STATE_CONNECTED;
    passive.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    utp_send_control_set_connected(&passive.send_control, true);
    active.local_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL] = 1u;

    REQUIRE(utp_connection_create_stream_internal(&passive, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 1u);
    stream = utp_connection_find_stream_internal(&passive, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_close_internal(stream) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 100u, &receive_pool);

    REQUIRE(utp_connection_create_stream_internal(&passive, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 5u);
    stream = utp_connection_find_stream_internal(&passive, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_close_internal(stream) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&passive);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_packet_in_pool_acquire(&receive_pool, &wire) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_flatten(packet, wire->data, wire->capacity, &length) == UTP_INTERNAL_ERROR_OK);
    wire->length = static_cast<uint16_t>(length);
    REQUIRE(utp_connection_on_packet_sent(&passive, packet, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_in_received(&active, wire, &passive_address, 200u) ==
            UTP_INTERNAL_ERROR_STREAM_LIMIT);
    utp_packet_in_release(wire);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
}
