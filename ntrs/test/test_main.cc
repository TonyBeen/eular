#define CATCH_CONFIG_MAIN
#include <ntrs/auth.h>
#include <ntrs/binary_protocol.h>
#include <ntrs/ntrs.h>
#include <ntrs/codec.h>
#include <ntrs/hub_state.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "catch/catch.hpp"

namespace {

static void PutU16Be(uint8_t* p, uint16_t value)
{
    p[0] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    p[1] = static_cast<uint8_t>(value & 0xFFu);
}

static void PutU32Be(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(value & 0xFFu);
}

}  // namespace

TEST_CASE("MessageAddBytesByTag failure leaves message unchanged")
{
    eular::ntrs::Message msg;
    uint8_t              data[eular::ntrs::Message::STORAGE_SIZE];

    memset(data, 0xA5, sizeof(data));
    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::AUTH_REQ, 1);

    REQUIRE_FALSE(eular::ntrs::MessageAddBytesByTag(&msg, eular::ntrs::FieldTag::TOKEN, data, sizeof(data)));
    REQUIRE(msg.field_count == 0);
    REQUIRE(msg.storage_len == 0);
    REQUIRE(eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::TOKEN)[0] == '\0');
}

TEST_CASE("DecodeMessage failure leaves no partial field")
{
    uint8_t              frame[eular::ntrs::FRAME_HDR_SIZE + eular::ntrs::TLV_HDR_SIZE + 1];
    eular::ntrs::Message msg;

    memset(frame, 0, sizeof(frame));
    PutU32Be(frame, eular::ntrs::FRAME_MAGIC);
    frame[4] = eular::ntrs::FRAME_VERSION;
    frame[5] = static_cast<uint8_t>(eular::ntrs::MessageType::AUTH_RSP);
    PutU32Be(frame + 12, eular::ntrs::TLV_HDR_SIZE + 1);
    PutU16Be(frame + eular::ntrs::FRAME_HDR_SIZE, static_cast<uint16_t>(eular::ntrs::FieldTag::LEASE_DEFAULT_SEC));
    PutU16Be(frame + eular::ntrs::FRAME_HDR_SIZE + 2, 1);
    frame[eular::ntrs::FRAME_HDR_SIZE + eular::ntrs::TLV_HDR_SIZE] = 30;

    REQUIRE_FALSE(eular::ntrs::DecodeMessage(frame, sizeof(frame), &msg));
    REQUIRE(msg.field_count == 0);
}

TEST_CASE("probe authorization uses keyed HMAC and requires a secret")
{
    std::string auth = eular::ntrs::MintProbeAuthorization("secret", "peer-a", "192.0.2.10", 33478, "token", 12345);
    std::string tampered = auth;
    tampered[tampered.size() - 1] = tampered[tampered.size() - 1] == '0' ? '1' : '0';

    REQUIRE(auth.size() == 64);
    REQUIRE(auth == "be59f5dc6243bdb3c3c6612ca82c137b3e9c40fa39c5605fa95cdedc91b1cd80");
    REQUIRE(auth != eular::ntrs::MintProbeAuthorization("other-secret", "peer-a", "192.0.2.10", 33478, "token", 12345));
    REQUIRE(eular::ntrs::ValidateProbeAuthorization("secret", "peer-a", "192.0.2.10", 33478, "token", 12345, auth));
    REQUIRE_FALSE(
        eular::ntrs::ValidateProbeAuthorization("secret", "peer-a", "192.0.2.10", 33478, "token", 12345, tampered));
    REQUIRE(eular::ntrs::MintProbeAuthorization("", "peer-a", "192.0.2.10", 33478, "token", 12345).empty());
    REQUIRE_FALSE(eular::ntrs::ValidateProbeAuthorization("", "peer-a", "192.0.2.10", 33478, "token", 12345, auth));
}

TEST_CASE("binary endpoint TLV preserves IPv6 address and port")
{
    uint8_t                 buffer[256];
    ntrs_binary_frame_t     frame;
    ntrs_binary_frame_view_t view;
    ntrs_binary_tlv_view_t  tlv;
    struct sockaddr_in6     input;
    struct sockaddr_storage output;
    socklen_t               output_len = 0;
    const struct sockaddr_in6* parsed = NULL;

    memset(&input, 0, sizeof(input));
    input.sin6_family = AF_INET6;
    input.sin6_port = htons(33478);
    REQUIRE(inet_pton(AF_INET6, "2001:db8::1234", &input.sin6_addr) == 1);

    REQUIRE(ntrs_binary_frame_init(&frame, buffer, sizeof(buffer)));
    REQUIRE(ntrs_binary_frame_set_header(&frame, NTRS_BINARY_FRAME_PROBE_RSP, NTRS_BINARY_PHASE_PROBE1, 0, 1, 0, 0));
    REQUIRE(ntrs_binary_frame_add_endpoint_tlv(&frame, NTRS_BINARY_TLV_MAPPED_ADDR,
                                               reinterpret_cast<const struct sockaddr*>(&input), sizeof(input)));

    REQUIRE(ntrs_binary_frame_parse(frame.buffer, frame.length, &view));
    REQUIRE(ntrs_binary_frame_find_tlv(&view, NTRS_BINARY_TLV_MAPPED_ADDR, &tlv));
    REQUIRE(ntrs_binary_tlv_parse_endpoint(&tlv, &output, &output_len));
    REQUIRE(output.ss_family == AF_INET6);
    REQUIRE(output_len == sizeof(struct sockaddr_in6));

    parsed = reinterpret_cast<const struct sockaddr_in6*>(&output);
    REQUIRE(ntohs(parsed->sin6_port) == 33478);
    REQUIRE(memcmp(&parsed->sin6_addr, &input.sin6_addr, sizeof(input.sin6_addr)) == 0);
}

TEST_CASE("Open public with firewall NAT class is a distinct generic class")
{
    REQUIRE(NTRS_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL != NTRS_NAT_CLASS_UNKNOWN);
    REQUIRE(NTRS_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL != NTRS_NAT_CLASS_OPEN_PUBLIC);
    REQUIRE(NTRS_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL != NTRS_NAT_CLASS_FULL_CONE);
}

TEST_CASE("HubClusterState applies direct node register messages by node id")
{
    eular::ntrs::HubClusterState      state;
    eular::ntrs::Message              msg;
    std::string                       event_name;
    eular::ntrs::ClusterNodeState     event_node;

    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::NODE_REGISTER, 1);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, "node-a");
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::BOOT_ID, "100-1");
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::REGION, "cn");
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::PROBE_ENDPOINT, "203.0.113.10:33478");
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::CONTROL_ENDPOINT, "203.0.113.10:19000");
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NAT_TYPE, "service_node");
    eular::ntrs::MessageAddU32ByTag(&msg, eular::ntrs::FieldTag::HEARTBEAT_INTERVAL_SEC, 5);

    REQUIRE(state.applyMessage("node-a", msg, 10, "2026-06-24T00:00:00Z", &event_name, &event_node));
    REQUIRE(event_name == "node_registered");
    REQUIRE(event_node.node_id == "node-a");
    REQUIRE(event_node.control_endpoint == "203.0.113.10:19000");
    REQUIRE(state.nodes().size() == 1);
    REQUIRE(state.nodes().find("node-a") != state.nodes().end());
}

TEST_CASE("HubClusterState rejects mismatched direct node id")
{
    eular::ntrs::HubClusterState  state;
    eular::ntrs::Message          msg;

    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::NODE_REGISTER, 1);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, "node-b");
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::BOOT_ID, "100-1");

    REQUIRE_FALSE(state.applyMessage("node-a", msg, 10, "2026-06-24T00:00:00Z", NULL, NULL));
    REQUIRE(state.nodes().empty());
}

TEST_CASE("HubClusterState raw node snapshot can exceed Message storage size")
{
    std::map<std::string, eular::ntrs::ClusterNodeState> nodes;

    for (int i = 0; i < 100; ++i) {
        char suffix[16];
        snprintf(suffix, sizeof(suffix), "%03d", i);

        eular::ntrs::ClusterNodeState node;
        node.node_id = std::string("node-very-long-identifier-for-capacity-test-") + suffix;
        node.boot_id = std::string("1782220000-") + suffix;
        node.status = "online";
        node.region = "capacity-test-region-with-long-name";
        node.probe_endpoint = std::string("[2408:8256:d178:481d:60bd:5f44:38a6:") + suffix + "]:33478";
        node.control_endpoint = std::string("[2408:8256:d178:481d:60bd:5f44:38a6:") + suffix + "]:19000";
        node.nat_type = "open_public_with_firewall";
        node.last_heartbeat = "2026-06-24T00:00:00Z";
        node.heartbeat_interval_sec = 5;
        node.last_seen_mono_sec = 1000u + (uint64_t)i;
        node.load = i;
        nodes[node.node_id] = node;
    }

    std::vector<uint8_t> payload;
    REQUIRE(eular::ntrs::EncodeClusterSnapshotNodes(nodes, &payload));
    REQUIRE(payload.size() > sizeof(static_cast<eular::ntrs::Message*>(NULL)->storage));

    std::vector<eular::ntrs::ClusterNodeState> decoded;
    REQUIRE(eular::ntrs::DecodeClusterSnapshotNodes(payload.data(), payload.size(), &decoded));
    REQUIRE(decoded.size() == nodes.size());

    eular::ntrs::HubClusterState restored;
    REQUIRE(restored.restoreFromNodes(1234, decoded));
    REQUIRE(restored.clusterVersion() == 1234);
    REQUIRE(restored.nodes().size() == nodes.size());
    REQUIRE(restored.nodes().find("node-very-long-identifier-for-capacity-test-099") != restored.nodes().end());
}
