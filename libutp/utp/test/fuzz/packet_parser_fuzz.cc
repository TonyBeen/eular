#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "proto/frame.h"
#include "proto/proto.h"
}

namespace {

constexpr size_t kFuzzPacketCapacity = 2048u;

static void      fuzz_walk_packet(const uint8_t* packet, size_t packet_length)
{
    utp_packet_view_t    view   = {};
    size_t               offset = 0u;
    utp_internal_error_t error;

    (void)utp_proto_decode_header(&view.header, packet, packet_length);
    error = utp_packet_view_decode(&view, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return;
    }
    while (offset < view.payload_length) {
        const uint8_t* frame_data;
        size_t         frame_length;
        uint8_t        frame_type;

        if (utp_packet_view_next_frame(&view, &offset, &frame_type, &frame_data, &frame_length) !=
            UTP_INTERNAL_ERROR_OK) {
            return;
        }
    }
}

static void fuzz_wrap_payload(const uint8_t* data, size_t size)
{
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + kFuzzPacketCapacity> packet = {};
    utp_packet_header_t header = {UINT32_C(1),          UINT32_C(2), UINT64_C(3), static_cast<uint16_t>(size),
                                  UTP_PACKET_TYPE_CTRL, UINT8_C(0)};
    size_t              index;

    if (utp_proto_encode_header(packet.data(), packet.size(), &header) != UTP_INTERNAL_ERROR_OK) {
        return;
    }
    for (index = 0u; index < size; ++index) {
        packet[UTP_PACKET_HEADER_SIZE + index] = data[index];
    }
    fuzz_walk_packet(packet.data(), UTP_PACKET_HEADER_SIZE + size);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (data == nullptr) {
        return 0;
    }
    if ((size != 0u && (data[0] & 1u) != 0u) || size > kFuzzPacketCapacity) {
        fuzz_walk_packet(data, size);
    } else {
        fuzz_wrap_payload(data, size);
    }
    return 0;
}
