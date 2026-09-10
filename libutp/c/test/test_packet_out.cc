#define CATCH_CONFIG_MAIN

#include <array>
#include <cstdlib>
#include <cstring>

#include <catch2/catch.hpp>

extern "C" {
#include "crypto/crypto.h"
#include "proto/frame.h"
#include "proto/packet_out.h"
#include "proto/proto.h"
}

namespace {

struct allocation_tracker {
    size_t allocations = 0u;
    size_t frees       = 0u;
};

void* tracked_alloc(void* user_data, size_t size)
{
    auto* tracker = static_cast<allocation_tracker*>(user_data);

    ++tracker->allocations;
    return std::malloc(size);
}

void* tracked_realloc(void* user_data, void* pointer, size_t size)
{
    auto* tracker = static_cast<allocation_tracker*>(user_data);

    ++tracker->allocations;
    return std::realloc(pointer, size);
}

void tracked_free(void* user_data, void* pointer)
{
    auto* tracker = static_cast<allocation_tracker*>(user_data);

    ++tracker->frees;
    std::free(pointer);
}

}  // namespace

TEST_CASE("packet_out pools allocate no storage during initialization", "[packet_out][pool]")
{
    allocation_tracker           tracker   = {};
    const utp_allocator_t        allocator = {tracked_alloc, tracked_realloc, tracked_free, &tracker};
    utp_packet_out_pool_t        packets   = {};
    utp_packet_out_buffer_pool_t buffers   = {};

    REQUIRE(utp_packet_out_pool_init(&packets, &allocator) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_buffer_pool_init(&buffers, &allocator) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(tracker.allocations == 0u);

    utp_packet_out_pool_cleanup(&packets);
    utp_packet_out_buffer_pool_cleanup(&buffers);
    REQUIRE(tracker.frees == 0u);
}

TEST_CASE("packet_out pools reject invalid initialization", "[packet_out][pool]")
{
    allocation_tracker           tracker             = {};
    utp_packet_out_pool_t        packets             = {};
    utp_packet_out_buffer_pool_t buffers             = {};
    utp_allocator_t              malformed_allocator = {nullptr, tracked_realloc, tracked_free, &tracker};

    REQUIRE(utp_packet_out_pool_init(nullptr, nullptr) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_buffer_pool_init(nullptr, nullptr) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&packets, &malformed_allocator) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_buffer_pool_init(&buffers, &malformed_allocator) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("packet_out acquire uses the fixed smallest fitting bucket", "[packet_out][acquire]")
{
    utp_packet_out_pool_t        packets = {};
    utp_packet_out_buffer_pool_t buffers = {};
    utp_packet_out_t*            packet  = nullptr;

    REQUIRE(utp_packet_out_pool_init(&packets, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_buffer_pool_init(&buffers, nullptr) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_packet_out_pool_acquire(&packets, &buffers, 1280u, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet->alloc_size == 1280u);
    utp_packet_out_pool_release(&packets, &buffers, packet);
    REQUIRE(utp_packet_out_pool_acquire(&packets, &buffers, 1281u, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet->alloc_size == 1500u);
    utp_packet_out_pool_release(&packets, &buffers, packet);
    REQUIRE(utp_packet_out_pool_acquire(&packets, &buffers, 65535u, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet->alloc_size == 65535u);
    REQUIRE(packet->raw_data == packet->encrypt_data);
    REQUIRE(packet->loss_chain == packet);
    utp_packet_out_pool_release(&packets, &buffers, packet);
    REQUIRE(utp_packet_out_pool_acquire(&packets, &buffers, 0u, &packet) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);

    utp_packet_out_pool_cleanup(&packets);
    utp_packet_out_buffer_pool_cleanup(&buffers);
}

TEST_CASE("packet_out pools grow in fixed batches", "[packet_out][pool]")
{
    allocation_tracker                tracker   = {};
    const utp_allocator_t             allocator = {tracked_alloc, tracked_realloc, tracked_free, &tracker};
    utp_packet_out_pool_t             packets   = {};
    utp_packet_out_buffer_pool_t      buffers   = {};
    std::array<utp_packet_out_t*, 33> acquired  = {};

    REQUIRE(utp_packet_out_pool_init(&packets, &allocator) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_buffer_pool_init(&buffers, &allocator) == UTP_INTERNAL_ERROR_OK);
    for (auto& packet : acquired) {
        REQUIRE(utp_packet_out_pool_acquire(&packets, &buffers, 1200u, &packet) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(packets.allocated_count == 64u);
    REQUIRE(buffers.buckets[0].allocated_count == 64u);
    REQUIRE(tracker.allocations == 4u);

    for (auto* packet : acquired) {
        utp_packet_out_pool_release(&packets, &buffers, packet);
    }
    utp_packet_out_pool_cleanup(&packets);
    utp_packet_out_buffer_pool_cleanup(&buffers);
    REQUIRE(tracker.frees == 4u);
}

TEST_CASE("packet_out buffer pool is shared by connections", "[packet_out][pool]")
{
    utp_packet_out_pool_t        first_pool  = {};
    utp_packet_out_pool_t        second_pool = {};
    utp_packet_out_buffer_pool_t buffers     = {};
    utp_packet_out_t*            first       = nullptr;
    utp_packet_out_t*            second      = nullptr;
    uint8_t*                     data;

    REQUIRE(utp_packet_out_pool_init(&first_pool, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_init(&second_pool, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_buffer_pool_init(&buffers, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&first_pool, &buffers, 1200u, &first) == UTP_INTERNAL_ERROR_OK);
    data = first->raw_data;
    utp_packet_out_pool_release(&first_pool, &buffers, first);
    REQUIRE(utp_packet_out_pool_acquire(&second_pool, &buffers, 1200u, &second) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(buffers.buckets[0].allocated_count == UTP_PACKET_OUT_GROW_COUNT);
    REQUIRE(second->raw_data != nullptr);
    REQUIRE(data != nullptr);

    utp_packet_out_pool_release(&second_pool, &buffers, second);
    utp_packet_out_pool_cleanup(&first_pool);
    utp_packet_out_pool_cleanup(&second_pool);
    utp_packet_out_buffer_pool_cleanup(&buffers);
}

TEST_CASE("packet_out pool release resets state but preserves the buffer for reuse", "[packet_out][release]")
{
    utp_packet_out_pool_t        packets = {};
    utp_packet_out_buffer_pool_t buffers = {};
    utp_packet_out_t*            pkt     = nullptr;
    utp_packet_out_t*            pkt2    = nullptr;
    uint8_t*                     original_raw_data;
    uint16_t                     original_alloc_size;

    REQUIRE(utp_packet_out_pool_init(&packets, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_buffer_pool_init(&buffers, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&packets, &buffers, 100u, &pkt) == UTP_INTERNAL_ERROR_OK);

    pkt->po_flags         = UTP_PO_ENCRYPTED;
    pkt->local_flags      = UTP_POL_LOSS;
    pkt->frame_types      = 0xffu;
    pkt->slice_count      = 3u;
    pkt->frame_meta_count = 2u;
    pkt->attempt_count    = 1u;
    original_raw_data     = pkt->raw_data;
    original_alloc_size   = pkt->alloc_size;

    utp_packet_out_pool_release(&packets, &buffers, pkt);
    REQUIRE(utp_packet_out_pool_acquire(&packets, &buffers, 100u, &pkt2) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(pkt2->raw_data == original_raw_data);
    REQUIRE(pkt2->alloc_size == original_alloc_size);
    REQUIRE(pkt2->po_flags == 0u);
    REQUIRE(pkt2->local_flags == 0u);
    REQUIRE(pkt2->frame_types == 0u);
    REQUIRE(pkt2->slice_count == 0u);
    REQUIRE(pkt2->frame_meta_count == 0u);
    REQUIRE(pkt2->attempt_count == 0u);
    REQUIRE(pkt2->loss_chain == pkt2);

    utp_packet_out_pool_release(&packets, &buffers, pkt2);
    utp_packet_out_pool_cleanup(&packets);
    utp_packet_out_buffer_pool_cleanup(&buffers);
}

TEST_CASE("packet_out buffer pool releases fully idle growth blocks", "[packet_out][pool]")
{
    utp_packet_out_pool_t             packets  = {};
    utp_packet_out_buffer_pool_t      buffers  = {};
    std::array<utp_packet_out_t*, 33> acquired = {};
    utp_packet_out_t*                 packet   = nullptr;
    auto&                             bucket   = buffers.buckets[0];

    REQUIRE(utp_packet_out_pool_init(&packets, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_buffer_pool_init(&buffers, nullptr) == UTP_INTERNAL_ERROR_OK);
    for (auto& item : acquired) {
        REQUIRE(utp_packet_out_pool_acquire(&packets, &buffers, 100u, &item) == UTP_INTERNAL_ERROR_OK);
    }
    for (auto* item : acquired) {
        utp_packet_out_pool_release(&packets, &buffers, item);
    }
    REQUIRE(bucket.allocated_count == 64u);

    bucket.sample_calls       = 1023u;
    bucket.sample_max_in_use  = 0u;
    bucket.sample_max_average = 1u;
    REQUIRE(utp_packet_out_pool_acquire(&packets, &buffers, 100u, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(bucket.allocated_count == 32u);
    utp_packet_out_pool_release(&packets, &buffers, packet);

    utp_packet_out_pool_cleanup(&packets);
    utp_packet_out_buffer_pool_cleanup(&buffers);
}

TEST_CASE("packet_out flatten joins raw and external slices", "[packet_out][slice]")
{
    utp_packet_out_t        packet   = {};
    std::array<uint8_t, 8>  raw      = {'h', 'e', 'a', 'd', 'e', 'r', 0, 0};
    std::array<uint8_t, 4>  external = {'d', 'a', 't', 'a'};
    std::array<uint8_t, 16> wire     = {};
    size_t                  length   = 0u;

    packet.raw_data    = raw.data();
    packet.alloc_size  = static_cast<uint16_t>(raw.size());
    packet.data_size   = 10u;
    packet.slice_count = 2u;
    packet.slices[0]   = {0u, 6u, nullptr, UTP_PACKET_OUT_SLICE_RAW_OFFSET};
    packet.slices[1]   = {0u, 4u, external.data(), UTP_PACKET_OUT_SLICE_EXTERNAL};

    REQUIRE(utp_packet_out_flatten(&packet, wire.data(), wire.size(), &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 10u);
    REQUIRE(std::memcmp(wire.data(), "headerdata", 10u) == 0);
}

TEST_CASE("packet_out strips a transient prefix without moving external stream data", "[packet_out][slice]")
{
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + 2u + UTP_FRAME_STREAM_HEADER_SIZE> raw         = {};
    std::array<uint8_t, 4u>                                                         stream_data = {'d', 'a', 't', 'a'};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_FRAME_STREAM_HEADER_SIZE + stream_data.size()> wire = {};
    const utp_packet_header_t header  = {11u,
                                         22u,
                                         33u,
                                         static_cast<uint16_t>(2u + UTP_FRAME_STREAM_HEADER_SIZE + stream_data.size()),
                                         UTP_PACKET_TYPE_CTRL,
                                         0u};
    utp_packet_header_t       decoded = {};
    utp_packet_out_t          packet  = {};
    size_t                    length  = 0u;

    REQUIRE(utp_proto_encode_header(raw.data(), raw.size(), &header) == UTP_INTERNAL_ERROR_OK);
    raw[UTP_PACKET_HEADER_SIZE]     = UTP_FRAME_TYPE_ACK;
    raw[UTP_PACKET_HEADER_SIZE + 1] = 0u;
    REQUIRE(utp_frame_stream_header_encode(raw.data() + UTP_PACKET_HEADER_SIZE + 2u,
                                           raw.size() - UTP_PACKET_HEADER_SIZE - 2u, UTP_STREAM_FLAG_NONE, 5u, 7u,
                                           static_cast<uint16_t>(stream_data.size())) == UTP_INTERNAL_ERROR_OK);

    packet.raw_data            = raw.data();
    packet.alloc_size          = static_cast<uint16_t>(raw.size());
    packet.data_size           = static_cast<uint16_t>(raw.size() + stream_data.size());
    packet.frame_types         = UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) | UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM);
    packet.transient_ack_size  = 2u;
    packet.control_prefix_size = 2u;
    packet.slice_count         = 2u;
    packet.slices[0]           = {0u, static_cast<uint16_t>(raw.size()), nullptr, UTP_PACKET_OUT_SLICE_RAW_OFFSET};
    packet.slices[1]           = {0u, static_cast<uint16_t>(stream_data.size()), stream_data.data(),
                                  UTP_PACKET_OUT_SLICE_EXTERNAL};
    packet.frame_meta_count    = 2u;
    packet.frame_meta[0]       = {
        nullptr, 0u, UTP_PACKET_HEADER_SIZE, 2u, 0u, UTP_FRAME_TYPE_ACK, UTP_FRAME_META_TRANSIENT_ON_RETRANSMIT};
    packet.frame_meta[1] = {nullptr,
                            0u,
                            static_cast<uint16_t>(UTP_PACKET_HEADER_SIZE + 2u),
                            UTP_FRAME_STREAM_HEADER_SIZE,
                            0u,
                            UTP_FRAME_TYPE_STREAM,
                            0u};

    REQUIRE(utp_packet_out_strip_prefix(&packet, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet.data_size == UTP_PACKET_HEADER_SIZE + UTP_FRAME_STREAM_HEADER_SIZE + stream_data.size());
    REQUIRE(packet.slice_count == 3u);
    REQUIRE(packet.slices[2].source == UTP_PACKET_OUT_SLICE_EXTERNAL);
    REQUIRE(packet.slices[2].data == stream_data.data());
    REQUIRE(packet.frame_types == UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM));
    REQUIRE(packet.frame_meta_count == 1u);
    REQUIRE(packet.frame_meta[0].frame_type == UTP_FRAME_TYPE_STREAM);
    REQUIRE(utp_proto_decode_header(&decoded, raw.data(), UTP_PACKET_HEADER_SIZE) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(decoded.payload_length == UTP_FRAME_STREAM_HEADER_SIZE + stream_data.size());
    REQUIRE(utp_packet_out_flatten(&packet, wire.data(), wire.size(), &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == packet.data_size);
    REQUIRE(std::memcmp(wire.data() + UTP_PACKET_HEADER_SIZE, raw.data() + UTP_PACKET_HEADER_SIZE + 2u,
                        UTP_FRAME_STREAM_HEADER_SIZE) == 0);
    REQUIRE(std::memcmp(wire.data() + UTP_PACKET_HEADER_SIZE + UTP_FRAME_STREAM_HEADER_SIZE, stream_data.data(),
                        stream_data.size()) == 0);
}

TEST_CASE("stripping a transient prefix preserves encrypted packet size", "[packet_out]")
{
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + 3u> raw    = {};
    utp_packet_header_t                              header = {1u, 2u, 3u, 3u, UTP_PACKET_TYPE_CTRL, 0u};
    utp_packet_out_t                                 packet = {};

    REQUIRE(utp_proto_encode_header(raw.data(), raw.size(), &header) == UTP_INTERNAL_ERROR_OK);
    packet.raw_data          = raw.data();
    packet.alloc_size        = (uint16_t)raw.size();
    packet.data_size         = (uint16_t)raw.size();
    packet.encrypt_data_size = (uint16_t)(raw.size() + UTP_CRYPTO_AEAD_TAG_SIZE);
    packet.po_flags          = UTP_PO_ENCRYPTED;
    packet.slice_count       = 1u;
    packet.slices[0]         = {0u, (uint16_t)raw.size(), nullptr, UTP_PACKET_OUT_SLICE_RAW_OFFSET};

    REQUIRE(utp_packet_out_strip_prefix(&packet, 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet.data_size == raw.size() - 1u);
    REQUIRE(packet.encrypt_data_size == packet.data_size + UTP_CRYPTO_AEAD_TAG_SIZE);
}
