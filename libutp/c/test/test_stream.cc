#define CATCH_CONFIG_MAIN

#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch.hpp>

extern "C" {
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

}  // namespace

TEST_CASE("stream reassembles out-of-order frames and reports FIN after data is consumed", "[stream]")
{
    utp_packet_in_pool_t pool       = {};
    utp_stream_t         stream     = {};
    uint8_t              buffer[16] = {};
    size_t               length     = 0u;
    bool                 fin        = false;

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 2u, 128u) == UTP_INTERNAL_ERROR_OK);
    utp_stream_init(&stream, 0u);
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
    REQUIRE(utp_stream_read(&stream, buffer, sizeof(buffer), &length, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 10u);
    REQUIRE(std::memcmp(buffer, "helloworld", 10u) == 0);
    REQUIRE_FALSE(fin);
    REQUIRE(utp_stream_read(&stream, buffer, sizeof(buffer), &length, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 0u);
    REQUIRE(fin);
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
    REQUIRE(utp_stream_read(&stream, buffer, sizeof(buffer), &length, &fin) == UTP_INTERNAL_ERROR_OK);
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
    REQUIRE(utp_stream_acquire_read_view(&stream, &view) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.length == payload.size());
    REQUIRE(view.data == decoded.data);
    REQUIRE(std::memcmp(view.data, payload.data(), payload.size()) == 0);
    REQUIRE(utp_stream_commit_read_view(&stream, view.offset, view.length) == UTP_INTERNAL_ERROR_OK);
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

    REQUIRE(utp_stream_acquire_read_view(&stream, &view) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.offset == 0u);
    REQUIRE(view.length == 5u);
    REQUIRE_FALSE(view.fin);
    REQUIRE(view.data == decoded.data);
    REQUIRE(std::memcmp(view.data, data, view.length) == 0);
    REQUIRE(utp_stream_commit_read_view(&stream, view.offset, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet->in_use);
    REQUIRE(packet->ref_count == 1u);

    REQUIRE(utp_stream_acquire_read_view(&stream, &view) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.offset == 2u);
    REQUIRE(view.length == 3u);
    REQUIRE(std::memcmp(view.data, data + 2u, view.length) == 0);
    REQUIRE(utp_stream_commit_read_view(&stream, view.offset, view.length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE_FALSE(packet->in_use);
    REQUIRE(packet->ref_count == 0u);

    REQUIRE(utp_stream_acquire_read_view(&stream, &fin_view) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(fin_view.offset == 5u);
    REQUIRE(fin_view.length == 0u);
    REQUIRE(fin_view.data == nullptr);
    REQUIRE(fin_view.fin);
    REQUIRE(utp_stream_commit_read_view(&stream, fin_view.offset, fin_view.length) == UTP_INTERNAL_ERROR_OK);
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
    REQUIRE(utp_stream_write(&stream, data.data(), data.size(), false) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_build_frame(&stream, payload.data(), payload.size(), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size == data.size());
    REQUIRE_FALSE(fin);
    REQUIRE(utp_stream_commit_built_frame(&stream, stream_data_size, fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_send_in_flight_bytes(&stream) == data.size());
    REQUIRE(utp_stream_write(&stream, data.data(), 1u, false) == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    REQUIRE(utp_stream_on_packet_acked(&stream, stream_data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_send_in_flight_bytes(&stream) == 0u);
    REQUIRE(utp_stream_write(&stream, data.data(), 1u, false) == UTP_INTERNAL_ERROR_OK);
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
    REQUIRE(utp_stream_acquire_write_views(&stream, views, 2u, &view_count, &writable) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view_count == 1u);
    REQUIRE(writable == UTP_STREAM_SEND_BUFFER_CAPACITY);
    REQUIRE(views[0].data == stream.send_buffer);
    REQUIRE(views[0].length == UTP_STREAM_SEND_BUFFER_CAPACITY);
    std::memcpy(views[0].data, "abc", 3u);
    REQUIRE(utp_stream_commit_write_views(&stream, 3u, true) == UTP_INTERNAL_ERROR_OK);
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
    REQUIRE(utp_stream_acquire_write_views(&stream, views, 2u, &view_count, &writable) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view_count == 2u);
    REQUIRE(writable == UTP_STREAM_SEND_BUFFER_CAPACITY);
    REQUIRE(views[0].data == stream.send_buffer + UTP_STREAM_SEND_BUFFER_CAPACITY - 2u);
    REQUIRE(views[0].length == 2u);
    REQUIRE(views[1].data == stream.send_buffer);
    REQUIRE(views[1].length == UTP_STREAM_SEND_BUFFER_CAPACITY - 2u);
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
    REQUIRE(utp_stream_write(&stream, reinterpret_cast<const uint8_t*>("abc"), 3u, false) == UTP_INTERNAL_ERROR_OK);
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
    utp_stream_reset(&stream);
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

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 0u);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t*>("abc"), 3u, true) ==
            UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 100u, &receive_pool);
    REQUIRE(passive.recv_reassembly_fragment_count == 1u);
    REQUIRE(passive.recv_reassembly_memory_bytes >= 1280u);
    {
        utp_stream_t* stream = utp_connection_find_stream(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(utp_stream_send_in_flight_bytes(stream) == 3u);
    }

    REQUIRE(utp_connection_stream_read(&passive, stream_id, buffer, sizeof(buffer), &length, &fin) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 3u);
    REQUIRE(std::memcmp(buffer, "abc", 3u) == 0);
    REQUIRE_FALSE(fin);
    REQUIRE(passive.recv_reassembly_fragment_count == 0u);
    REQUIRE(passive.recv_reassembly_memory_bytes == 0u);
    REQUIRE(utp_connection_stream_read(&passive, stream_id, buffer, sizeof(buffer), &length, &fin) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 0u);
    REQUIRE(fin);
    REQUIRE(utp_connection_queue_ack(&passive, 200u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 200u, &receive_pool);
    {
        utp_stream_t* stream = utp_connection_find_stream(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(utp_stream_send_in_flight_bytes(stream) == 0u);
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
    utp_packet_in_pool_cleanup(&receive_pool);
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

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t*>("abcd"), 4u, false) ==
            UTP_INTERNAL_ERROR_OK);
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
    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 0u);
    active.peer_max_data = 1u;
    {
        utp_stream_t* stream = utp_connection_find_stream(&active, stream_id);

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
        utp_stream_t* stream = utp_connection_find_stream(&active, stream_id);

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
    REQUIRE(active.peer_max_stream_data_count == 1u);

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == max_stream_data.stream_id);
    {
        utp_stream_t* stream = utp_connection_find_stream(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(stream->peer_max_stream_data == max_stream_data.maximum_stream_data);
    }
    REQUIRE(active.peer_max_stream_data_count == 0u);

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

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t*>("x"), 1u, false) ==
            UTP_INTERNAL_ERROR_OK);
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
    {
        utp_stream_t* stream = utp_connection_find_stream(&passive, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(stream->recv_fragment_count == 0u);
        REQUIRE(stream->recv_pinned_memory_bytes == 0u);
    }

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

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t*>("abcde"), 5u, false) ==
            UTP_INTERNAL_ERROR_OK);

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

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t*>("x"), 1u, false) ==
            UTP_INTERNAL_ERROR_OK);
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

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t*>("abc"), 3u, false) ==
            UTP_INTERNAL_ERROR_OK);
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

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t*>("abcdefghij"), 10u,
                                        false) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 100u, &receive_pool);
    {
        utp_stream_t* stream = utp_connection_find_stream(&passive, stream_id);

        REQUIRE(stream != nullptr);
        passive.local_max_data_advertised        = connection_window - (connection_window / 10u);
        stream->local_max_stream_data_advertised = stream_window - (stream_window / 10u);
    }

    REQUIRE(utp_connection_stream_read(&passive, stream_id, buffer, sizeof(buffer), &length, &fin) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 10u);
    REQUIRE(passive.local_stream_data_consumed_total == 10u);

    {
        uint8_t                 frame_type;
        size_t                  frame_length;
        const uint8_t*          frame_data;
        utp_frame_max_data_t    max_data = {};
        utp_packet_out_t* const packet   = utp_connection_next_packet_to_send(&passive);

        frame_data = first_packet_frame(packet, &frame_type, &frame_length);
        REQUIRE(frame_type == UTP_FRAME_TYPE_MAX_DATA);
        REQUIRE(utp_frame_max_data_decode(&max_data, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(max_data.maximum_data == connection_window + 10u);
        REQUIRE(utp_connection_on_packet_sent(&passive, packet, 200u) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_send_control_unacked_packet_count(&passive.send_control) == 1u);
    }
    {
        uint8_t                     frame_type;
        size_t                      frame_length;
        const uint8_t*              frame_data;
        utp_frame_max_stream_data_t max_stream_data = {};
        utp_packet_out_t* const     packet          = utp_connection_next_packet_to_send(&passive);

        frame_data = first_packet_frame(packet, &frame_type, &frame_length);
        REQUIRE(frame_type == UTP_FRAME_TYPE_MAX_STREAM_DATA);
        REQUIRE(utp_frame_max_stream_data_decode(&max_stream_data, frame_data, frame_length) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(max_stream_data.stream_id == stream_id);
        REQUIRE(max_stream_data.maximum_stream_data == stream_window + 10u);
        REQUIRE(utp_connection_on_packet_sent(&passive, packet, 201u) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_send_control_unacked_packet_count(&passive.send_control) == 2u);
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

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t*>("abcd"), 4u, false) ==
            UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 100u, &receive_pool);
    passive.local_max_data_advertised = connection_window - (connection_window / 10u);

    REQUIRE(utp_connection_stream_acquire_read_view(&passive, stream_id, &view) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.length == 4u);
    REQUIRE(utp_connection_stream_commit_read_view(&passive, stream_id, view.offset, view.length) ==
            UTP_INTERNAL_ERROR_OK);
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
