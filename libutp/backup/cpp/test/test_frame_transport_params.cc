/*************************************************************************
    > File Name: test_frame_transport_params.cc
    > Author: eular
    > Brief:
    > Created Time: Tue 01 Apr 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <array>

#include "proto/frame/transport_params.h"

using eular::utp::FrameTransportParams;
using eular::utp::TransportParams;
using eular::utp::Status;

TEST_CASE("TransportParams frame: encode/decode", "[FrameTransportParams]")
{
    TransportParams localTp;
    localTp.flags = TransportParams::kDefaultFlags;
    localTp.max_idle_timeout = 12345;
    localTp.handshake_timeout = 2345;
    localTp.init_max_streams_bidi = 48;
    localTp.init_max_streams_uni = 24;
    localTp.ack_delay_exponent = 4;
    localTp.initial_max_data = 7ull * 1024ull * 1024ull;
    localTp.initial_max_stream_data_bidi_local = 3ull * 1024ull * 1024ull;
    localTp.initial_max_stream_data_bidi_remote = 5ull * 1024ull * 1024ull;

    FrameTransportParams frame;
    frame.params = &localTp;

    std::array<uint8_t, FRAME_TRANSPORT_PARAMS_SIZE> buffer{};
    Status st;
    int32_t encoded = frame.encode(buffer.data(), buffer.size(), st);
    REQUIRE(st.ok());
    REQUIRE(encoded == FRAME_TRANSPORT_PARAMS_SIZE);

    TransportParams peerTp;
    FrameTransportParams decoded;
    decoded.params = &peerTp;

    int32_t decodedLen = decoded.decode(buffer.data(), static_cast<size_t>(encoded), st);
    REQUIRE(st.ok());
    REQUIRE(decodedLen == encoded);
    REQUIRE(peerTp.flags == localTp.flags);
    REQUIRE(peerTp.max_idle_timeout == localTp.max_idle_timeout);
    REQUIRE(peerTp.handshake_timeout == localTp.handshake_timeout);
    REQUIRE(peerTp.init_max_streams_bidi == localTp.init_max_streams_bidi);
    REQUIRE(peerTp.init_max_streams_uni == localTp.init_max_streams_uni);
    REQUIRE(peerTp.ack_delay_exponent == localTp.ack_delay_exponent);
    REQUIRE(peerTp.initial_max_data == localTp.initial_max_data);
    REQUIRE(peerTp.initial_max_stream_data_bidi_local == localTp.initial_max_stream_data_bidi_local);
    REQUIRE(peerTp.initial_max_stream_data_bidi_remote == localTp.initial_max_stream_data_bidi_remote);
}

TEST_CASE("TransportParams frame rejects unsafe numeric values", "[FrameTransportParams][Regression]")
{
    auto encodeAndDecode = [](const TransportParams& input, Status& decodeStatus) {
        TransportParams encodedParams = input;
        FrameTransportParams encoder;
        encoder.params = &encodedParams;
        std::array<uint8_t, FRAME_TRANSPORT_PARAMS_SIZE> bytes{};
        Status encodeStatus;
        REQUIRE(encoder.encode(bytes.data(), bytes.size(), encodeStatus) == FRAME_TRANSPORT_PARAMS_SIZE);

        TransportParams decodedParams;
        FrameTransportParams decoder;
        decoder.params = &decodedParams;
        return decoder.decode(bytes.data(), bytes.size(), decodeStatus);
    };

    SECTION("ACK delay exponent") {
        TransportParams params;
        params.ack_delay_exponent = TransportParams::kMaxAckDelayExponent + 1;
        Status st;
        REQUIRE(encodeAndDecode(params, st) < 0);
        REQUIRE(st.code() == UTP_ERR_INVALID_PARAM);
    }

    SECTION("flow-control value") {
        TransportParams params;
        params.initial_max_data = TransportParams::kMaxFlowControlValue + 1;
        Status st;
        REQUIRE(encodeAndDecode(params, st) < 0);
        REQUIRE(st.code() == UTP_ERR_INVALID_PARAM);
    }

    SECTION("unknown flag") {
        TransportParams params;
        params.flags |= (1u << 15);
        Status st;
        REQUIRE(encodeAndDecode(params, st) < 0);
        REQUIRE(st.code() == UTP_ERR_INVALID_PARAM);
    }
}
