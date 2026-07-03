#include <ntrs_auth.h>
#include <ntrs_client.h>
#include <ntrs_codec.h>
#include <ntrs_hub_state.h>
#include <ntrs_io.h>
#include <socket_address.h>
#include <stun.h>
#include <stun_types.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <event2/event.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "catch/catch.hpp"

TEST_CASE("typed tlv roundtrip preserves typed values", "[ntrs][codec]")
{
    eular::ntrs::Message msg;
    uint8_t              buffer[2048];
    size_t               encoded_len = 0;
    eular::ntrs::Message decoded;
    bool                 bool_value = false;
    uint16_t             u16_value = 0;
    uint32_t             u32_value = 0;
    int32_t              i32_value = 0;
    uint64_t             u64_value = 0;

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::REGISTER_REQ, 7);
    REQUIRE(eular::ntrs::messageAddBool(&msg, "mapping_stable", true));
    REQUIRE(eular::ntrs::messageAddU16(&msg, "local_port", 4231));
    REQUIRE(eular::ntrs::messageAddU32(&msg, "probe_rounds", 9));
    REQUIRE(eular::ntrs::messageAddI32(&msg, "probe1_rtt_ms", -17));
    REQUIRE(eular::ntrs::messageAddU64(&msg, "cluster_version", 123456789ULL));
    REQUIRE(eular::ntrs::messageAddString(&msg, "peer_id", "peer_a"));

    REQUIRE(eular::ntrs::encodeMessage(msg, buffer, sizeof(buffer), &encoded_len) == 0);
    REQUIRE(eular::ntrs::decodeMessage(buffer, encoded_len, &decoded));
    REQUIRE(eular::ntrs::messageGetBool(&decoded, "mapping_stable", &bool_value));
    REQUIRE(bool_value);
    REQUIRE(eular::ntrs::messageGetU16(&decoded, "local_port", &u16_value));
    REQUIRE(u16_value == 4231);
    REQUIRE(eular::ntrs::messageGetU32(&decoded, "probe_rounds", &u32_value));
    REQUIRE(u32_value == 9);
    REQUIRE(eular::ntrs::messageGetI32(&decoded, "probe1_rtt_ms", &i32_value));
    REQUIRE(i32_value == -17);
    REQUIRE(eular::ntrs::messageGetU64(&decoded, "cluster_version", &u64_value));
    REQUIRE(u64_value == 123456789ULL);
    REQUIRE(std::string(eular::ntrs::messageGetString(&decoded, "peer_id")) == "peer_a");
}

TEST_CASE("hub state replaces older generation and ignores stale lwt", "[ntrs][hub]")
{
    eular::ntrs::HubClusterState  state;
    eular::ntrs::Message          msg;
    std::string                   event_name;
    eular::ntrs::ClusterNodeState event_node;
    const std::string             node_id = "node-a";

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::NODE_REGISTER, 1);
    eular::ntrs::messageAddString(&msg, "node_id", node_id.c_str());
    eular::ntrs::messageAddString(&msg, "boot_id", "100-1");
    eular::ntrs::messageAddString(&msg, "control_endpoint", "10.0.0.1:1000");
    eular::ntrs::messageAddU32(&msg, "heartbeat_interval_sec", 1);
    REQUIRE(state.applyMessage(node_id, msg, 100, "2026-01-01T00:00:00Z", &event_name, &event_node));
    REQUIRE(event_node.boot_id == "100-1");

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::NODE_PRESENCE, 2);
    eular::ntrs::messageAddString(&msg, "node_id", node_id.c_str());
    eular::ntrs::messageAddString(&msg, "boot_id", "100-1");
    eular::ntrs::messageAddString(&msg, "status", "online");
    REQUIRE(state.applyMessage(node_id, msg, 101, "2026-01-01T00:00:01Z", &event_name, &event_node));

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::NODE_REGISTER, 3);
    eular::ntrs::messageAddString(&msg, "node_id", node_id.c_str());
    eular::ntrs::messageAddString(&msg, "boot_id", "200-1");
    eular::ntrs::messageAddString(&msg, "control_endpoint", "10.0.0.2:1000");
    eular::ntrs::messageAddU32(&msg, "heartbeat_interval_sec", 1);
    REQUIRE(state.applyMessage(node_id, msg, 200, "2026-01-01T00:00:02Z", &event_name, &event_node));
    REQUIRE(event_name == "node_generation_replaced");
    REQUIRE(state.nodes().find(node_id)->second.boot_id == "200-1");

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::NODE_PRESENCE, 4);
    eular::ntrs::messageAddString(&msg, "node_id", node_id.c_str());
    eular::ntrs::messageAddString(&msg, "boot_id", "100-1");
    eular::ntrs::messageAddString(&msg, "status", "offline");
    eular::ntrs::messageAddString(&msg, "reason", "lwt");
    REQUIRE_FALSE(state.applyMessage(node_id, msg, 201, "2026-01-01T00:00:03Z", &event_name, &event_node));
    REQUIRE(state.nodes().find(node_id)->second.boot_id == "200-1");
    REQUIRE(state.nodes().find(node_id)->second.status != "offline");
}

TEST_CASE("cluster snapshot encodes structured node state and restores hub view", "[ntrs][hub][snapshot]")
{
    eular::ntrs::HubClusterState               state;
    eular::ntrs::Message                       msg;
    eular::ntrs::Message                       snapshot;
    std::string                                event_name;
    eular::ntrs::ClusterNodeState              event_node;
    uint64_t                                   cluster_version = 0;
    const uint8_t*                             snapshot_nodes = NULL;
    uint16_t                                   snapshot_nodes_len = 0;
    uint32_t                                   snapshot_node_count = 0;
    std::vector<eular::ntrs::ClusterNodeState> decoded_nodes;
    eular::ntrs::HubClusterState               restored;
    const std::string                          node_id = "node-c";

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::NODE_REGISTER, 1);
    eular::ntrs::messageAddString(&msg, "node_id", node_id.c_str());
    eular::ntrs::messageAddString(&msg, "boot_id", "400-1");
    eular::ntrs::messageAddString(&msg, "region", "cn-sh");
    eular::ntrs::messageAddString(&msg, "stun_endpoint", "10.0.0.1:3478");
    eular::ntrs::messageAddString(&msg, "control_endpoint", "10.0.0.1:19000");
    eular::ntrs::messageAddString(&msg, "nat_type", "full_cone_nat");
    eular::ntrs::messageAddU32(&msg, "heartbeat_interval_sec", 7);
    REQUIRE(state.applyMessage(node_id, msg, 400, "2026-01-01T00:00:00Z", &event_name, &event_node));

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::NODE_HEARTBEAT, 2);
    eular::ntrs::messageAddString(&msg, "node_id", node_id.c_str());
    eular::ntrs::messageAddString(&msg, "boot_id", "400-1");
    eular::ntrs::messageAddString(&msg, "ts", "2026-01-01T00:00:03Z");
    eular::ntrs::messageAddU32(&msg, "heartbeat_interval_sec", 7);
    eular::ntrs::messageAddU32(&msg, "load", 23);
    REQUIRE(state.applyMessage(node_id, msg, 403, "2026-01-01T00:00:03Z", &event_name, &event_node));
    REQUIRE(state.nodes().find(node_id)->second.nat_type == "full_cone_nat");

    REQUIRE(eular::ntrs::buildClusterSnapshotMessage(state.nodes(), state.cluster_version(), "2026-01-01T00:00:04Z",
                                                     &snapshot));
    REQUIRE(eular::ntrs::messageGetU32(&snapshot, "node_count", &snapshot_node_count));
    REQUIRE(snapshot_node_count == 1);
    REQUIRE(eular::ntrs::messageGetBytes(&snapshot, "nodes", &snapshot_nodes, &snapshot_nodes_len));
    REQUIRE(snapshot_nodes != NULL);
    REQUIRE(snapshot_nodes_len > 3);
    REQUIRE(eular::ntrs::decodeClusterSnapshotNodes(snapshot_nodes, snapshot_nodes_len, &decoded_nodes));
    decoded_nodes.clear();
    REQUIRE(eular::ntrs::parseClusterSnapshotMessage(snapshot, &cluster_version, &decoded_nodes));
    REQUIRE(cluster_version == state.cluster_version());
    REQUIRE(decoded_nodes.size() == 1);
    REQUIRE(decoded_nodes[0].node_id == node_id);
    REQUIRE(decoded_nodes[0].boot_id == "400-1");
    REQUIRE(decoded_nodes[0].region == "cn-sh");
    REQUIRE(decoded_nodes[0].stun_endpoint == "10.0.0.1:3478");
    REQUIRE(decoded_nodes[0].control_endpoint == "10.0.0.1:19000");
    REQUIRE(decoded_nodes[0].nat_type == "full_cone_nat");
    REQUIRE(decoded_nodes[0].last_heartbeat == "2026-01-01T00:00:03Z");
    REQUIRE(decoded_nodes[0].heartbeat_interval_sec == 7);
    REQUIRE(decoded_nodes[0].last_seen_mono_sec == 403);
    REQUIRE(decoded_nodes[0].load == 23);

    REQUIRE(restored.restoreFromSnapshot(snapshot));
    REQUIRE(restored.cluster_version() == state.cluster_version());
    REQUIRE(restored.nodes().size() == 1);
    REQUIRE(restored.nodes().find(node_id) != restored.nodes().end());
    REQUIRE(restored.nodes().find(node_id)->second.control_endpoint == "10.0.0.1:19000");
    REQUIRE_FALSE(restored.restoreFromSnapshot(snapshot));
}

TEST_CASE("hub evicts node after heartbeat timeout", "[ntrs][hub]")
{
    eular::ntrs::HubClusterState               state;
    eular::ntrs::Message                       msg;
    std::string                                event_name;
    eular::ntrs::ClusterNodeState              event_node;
    std::vector<eular::ntrs::ClusterNodeState> evicted_nodes;
    const std::string                          node_id = "node-b";

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::NODE_REGISTER, 1);
    eular::ntrs::messageAddString(&msg, "node_id", node_id.c_str());
    eular::ntrs::messageAddString(&msg, "boot_id", "300-1");
    eular::ntrs::messageAddU32(&msg, "heartbeat_interval_sec", 1);
    REQUIRE(state.applyMessage(node_id, msg, 300, "2026-01-01T00:00:00Z", &event_name, &event_node));

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::NODE_HEARTBEAT, 2);
    eular::ntrs::messageAddString(&msg, "node_id", node_id.c_str());
    eular::ntrs::messageAddString(&msg, "boot_id", "300-1");
    eular::ntrs::messageAddString(&msg, "ts", "2026-01-01T00:00:01Z");
    eular::ntrs::messageAddU32(&msg, "heartbeat_interval_sec", 1);
    REQUIRE(state.applyMessage(node_id, msg, 301, "2026-01-01T00:00:01Z", &event_name, &event_node));

    REQUIRE(state.sweepExpired(305, &evicted_nodes));
    REQUIRE(evicted_nodes.size() == 1);
    REQUIRE(state.nodes().empty());
}

TEST_CASE("auth manager enforces control and peer session scope", "[ntrs][auth]")
{
    eular::ntrs::ControlAuthManager auth("shared-secret", 30);
    eular::ntrs::ControlSession     control_session;
    eular::ntrs::PeerSessionLease   peer_session;
    std::string                     reason;
    std::string                     peer_sid;

    REQUIRE(auth.issueSession("peer-a", "shared-secret", 11, 1000, &control_session, &reason));
    REQUIRE(auth.validateSession(11, "peer-a", control_session.token, 1001, &reason));
    REQUIRE(auth.validateSession(11, "peer-a", control_session.token, 1029, &reason));
    REQUIRE(auth.validateSession(11, "peer-a", control_session.token, 1058, &reason));
    REQUIRE_FALSE(auth.validateSession(11, "peer-b", control_session.token, 1001, &reason));
    REQUIRE(reason == "peer_id mismatch");
    REQUIRE_FALSE(auth.validateSession(11, "peer-a", control_session.token, 1089, &reason));
    REQUIRE(reason == "session token expired");

    peer_sid = eular::ntrs::mintPeerSessionId("peer-a", "peer-b", 1000);
    REQUIRE(auth.issuePeerSession("peer-a", "peer-b", peer_sid, 1000, 60, &peer_session, &reason));
    REQUIRE(auth.validatePeerSession(peer_sid, "peer-a", "peer-b", peer_session.token, 1050, &reason));
    REQUIRE_FALSE(auth.validatePeerSession(peer_sid, "peer-a", "peer-c", peer_session.token, 1050, &reason));
    REQUIRE(reason == "peer session dst mismatch");
    REQUIRE_FALSE(auth.validatePeerSession(peer_sid, "peer-a", "peer-b", peer_session.token, 1060, &reason));
    REQUIRE(reason == "peer session token expired");
    auth.revokePeerSession(peer_sid);
    REQUIRE_FALSE(auth.validatePeerSession(peer_sid, "peer-a", "peer-b", peer_session.token, 1050, &reason));
    REQUIRE(reason == "peer session missing");
}

TEST_CASE("probe authorization binds target and owner scope", "[ntrs][auth][probe]")
{
    std::string authz =
        eular::ntrs::mintProbeAuthorization("shared-secret", "peer-a", "203.0.113.8", 41234, "probe-token", 12345);

    REQUIRE(eular::ntrs::validateProbeAuthorization("shared-secret", "peer-a", "203.0.113.8", 41234, "probe-token",
                                                    12345, authz));
    REQUIRE_FALSE(eular::ntrs::validateProbeAuthorization("shared-secret", "peer-b", "203.0.113.8", 41234,
                                                          "probe-token", 12345, authz));
    REQUIRE_FALSE(eular::ntrs::validateProbeAuthorization("shared-secret", "peer-a", "203.0.113.9", 41234,
                                                          "probe-token", 12345, authz));
    REQUIRE_FALSE(eular::ntrs::validateProbeAuthorization("shared-secret", "peer-a", "203.0.113.8", 41235,
                                                          "probe-token", 12345, authz));
    REQUIRE_FALSE(eular::ntrs::validateProbeAuthorization("shared-secret", "peer-a", "203.0.113.8", 41234,
                                                          "probe-token-2", 12345, authz));
}

TEST_CASE("recvMessageWithTimeout fails fast on partial frame", "[ntrs][io]")
{
    int                  sockets[2] = {-1, -1};
    eular::ntrs::Message msg;
    eular::ntrs::Message decoded;
    uint8_t              buffer[256];
    size_t               encoded_len = 0;
    auto                 start = std::chrono::steady_clock::now();
    auto                 elapsed_ms = 0LL;

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::AUTH_REQ, 19);
    REQUIRE(eular::ntrs::messageAddString(&msg, "peer_id", "slow-peer"));
    REQUIRE(eular::ntrs::encodeMessage(msg, buffer, sizeof(buffer), &encoded_len) == 0);
    REQUIRE(encoded_len > eular::ntrs::FRAME_HDR_SIZE);

    REQUIRE(write(sockets[0], buffer, eular::ntrs::FRAME_HDR_SIZE) == (ssize_t)eular::ntrs::FRAME_HDR_SIZE);

    REQUIRE_FALSE(eular::ntrs::recvMessageWithTimeout(sockets[1], 50, &decoded));
    elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(elapsed_ms >= 40);
    REQUIRE(elapsed_ms < 500);

    close(sockets[0]);
    close(sockets[1]);
}

TEST_CASE("connectTcpHostPort resolves localhost endpoints", "[ntrs][io]")
{
    int                listener = -1;
    int                accepted = -1;
    int                client = -1;
    struct sockaddr_in addr;
    socklen_t          addr_len = sizeof(addr);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    REQUIRE(bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(listener, 1) == 0);
    REQUIRE(getsockname(listener, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0);

    REQUIRE(eular::ntrs::connectTcpHostPort("localhost", ntohs(addr.sin_port), 200, &client));
    accepted = accept(listener, NULL, NULL);
    REQUIRE(accepted >= 0);

    close(accepted);
    close(client);
    close(listener);
}

TEST_CASE("async auth invokes callback with session token", "[ntrs][client][async]")
{
    struct AsyncAuthState {
        event_base*          base;
        bool*                done;
        ntrs_async_result_t* result;
    };

    int                 sockets[2] = {-1, -1};
    bool                done = false;
    bool                server_ok = true;
    ntrs_async_result_t callback_result;
    event_base*         base = event_base_new();

    REQUIRE(base != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    ntrs_async_client_t* client = ntrs_async_client_create(base);
    REQUIRE(client != NULL);

    auto callback = [](const ntrs_async_result_t* result, void* user_data) {
        AsyncAuthState* state = static_cast<AsyncAuthState*>(user_data);
        *state->done = true;
        *state->result = *result;
        event_base_loopbreak(state->base);
    };

    AsyncAuthState state = {base, &done, &callback_result};

    uint64_t request_id = 0;
    REQUIRE(ntrs_async_auth(client, &request_id, sockets[0], "peer-a", "bootstrap", callback, &state));
    REQUIRE(request_id != 0);

    REQUIRE(event_base_loop(base, EVLOOP_NONBLOCK) == 0);

    {
        eular::ntrs::Message req;
        eular::ntrs::Message rsp;
        uint8_t              buffer[256];
        size_t               encoded_len = 0;

        if (!eular::ntrs::recvMessageWithTimeout(sockets[1], 200, &req) ||
            req.type != eular::ntrs::MessageType::AUTH_REQ ||
            std::string(eular::ntrs::messageGetString(&req, "peer_id")) != "peer-a") {
            server_ok = false;
        }

        eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::AUTH_RSP, req.request_id);
        if (!eular::ntrs::messageAddString(&rsp, "result", "ok") ||
            !eular::ntrs::messageAddString(&rsp, "token", "session-token") ||
            !eular::ntrs::messageAddU32(&rsp, "lease_default_sec", 30) ||
            eular::ntrs::encodeMessage(rsp, buffer, sizeof(buffer), &encoded_len) != 0 ||
            write(sockets[1], buffer, encoded_len) != (ssize_t)encoded_len) {
            server_ok = false;
        }
    }

    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done && std::chrono::steady_clock::now() < deadline) {
            REQUIRE(event_base_loop(base, EVLOOP_ONCE) == 0);
        }
    }

    REQUIRE(done);
    REQUIRE(callback_result.request_id == request_id);
    REQUIRE(callback_result.type == NTRS_ASYNC_AUTH);
    REQUIRE(callback_result.success);
    REQUIRE_FALSE(callback_result.cancelled);
    REQUIRE(std::string(callback_result.session_token) == "session-token");
    REQUIRE(callback_result.lease_default_sec == 30);

    ntrs_async_client_destroy(client);
    event_base_free(base);
    REQUIRE(server_ok);
    close(sockets[0]);
    close(sockets[1]);
}

TEST_CASE("async heartbeat renews peer lease without blocking caller", "[ntrs][client][async]")
{
    struct AsyncHeartbeatState {
        event_base*          base;
        bool*                done;
        ntrs_async_result_t* result;
    };

    int                 sockets[2] = {-1, -1};
    bool                done = false;
    bool                server_ok = true;
    ntrs_async_result_t callback_result;
    event_base*         base = event_base_new();

    REQUIRE(base != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    ntrs_async_client_t* client = ntrs_async_client_create(base);
    REQUIRE(client != NULL);

    auto callback = [](const ntrs_async_result_t* result, void* user_data) {
        AsyncHeartbeatState* state = static_cast<AsyncHeartbeatState*>(user_data);
        *state->done = true;
        *state->result = *result;
        event_base_loopbreak(state->base);
    };

    AsyncHeartbeatState state = {base, &done, &callback_result};

    uint64_t request_id = 0;
    REQUIRE(ntrs_async_heartbeat(client, &request_id, sockets[0], "peer-a", "session-token", 42, callback, &state));
    REQUIRE(request_id != 0);

    REQUIRE(event_base_loop(base, EVLOOP_NONBLOCK) == 0);

    {
        eular::ntrs::Message req;
        eular::ntrs::Message rsp;
        uint8_t              buffer[256];
        size_t               encoded_len = 0;
        uint32_t             lease_seq = 0;

        if (!eular::ntrs::recvMessageWithTimeout(sockets[1], 200, &req) ||
            req.type != eular::ntrs::MessageType::HEARTBEAT_REQ ||
            std::string(eular::ntrs::messageGetString(&req, "peer_id")) != "peer-a" ||
            std::string(eular::ntrs::messageGetString(&req, "token")) != "session-token" ||
            !eular::ntrs::messageGetU32(&req, "lease_seq", &lease_seq) || lease_seq != 42) {
            server_ok = false;
        }

        eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::HEARTBEAT_RSP, req.request_id);
        if (!eular::ntrs::messageAddString(&rsp, "result", "ok") ||
            eular::ntrs::encodeMessage(rsp, buffer, sizeof(buffer), &encoded_len) != 0 ||
            write(sockets[1], buffer, encoded_len) != (ssize_t)encoded_len) {
            server_ok = false;
        }
    }

    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done && std::chrono::steady_clock::now() < deadline) {
            REQUIRE(event_base_loop(base, EVLOOP_ONCE) == 0);
        }
    }

    REQUIRE(done);
    REQUIRE(callback_result.request_id == request_id);
    REQUIRE(callback_result.type == NTRS_ASYNC_HEARTBEAT);
    REQUIRE(callback_result.success);
    REQUIRE_FALSE(callback_result.cancelled);

    ntrs_async_client_destroy(client);
    event_base_free(base);
    REQUIRE(server_ok);
    close(sockets[0]);
    close(sockets[1]);
}

TEST_CASE("async connect control resolves localhost without blocking caller", "[ntrs][client][async]")
{
    struct AsyncConnectState {
        event_base*          base;
        bool*                done;
        ntrs_async_result_t* result;
    };

    event_base*         base = event_base_new();
    int                 listener = -1;
    int                 accepted = -1;
    event*              accept_event = NULL;
    sockaddr_in         addr;
    socklen_t           addr_len = sizeof(addr);
    bool                done = false;
    ntrs_async_result_t callback_result;

    REQUIRE(base != NULL);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(listener, 1) == 0);
    REQUIRE(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) == 0);

    auto accept_cb = [](evutil_socket_t fd, short, void* user_data) {
        int* accepted_fd = static_cast<int*>(user_data);
        *accepted_fd = accept((int)fd, NULL, NULL);
    };
    accept_event = event_new(base, listener, EV_READ | EV_PERSIST, accept_cb, &accepted);
    REQUIRE(accept_event != NULL);
    REQUIRE(event_add(accept_event, NULL) == 0);

    ntrs_async_client_t* client = ntrs_async_client_create(base);
    REQUIRE(client != NULL);
    auto callback = [](const ntrs_async_result_t* result, void* user_data) {
        AsyncConnectState* state = static_cast<AsyncConnectState*>(user_data);
        *state->done = true;
        *state->result = *result;
        event_base_loopbreak(state->base);
    };

    AsyncConnectState state = {base, &done, &callback_result};
    uint64_t          request_id = 0;
    REQUIRE(ntrs_async_connect_control(client, &request_id, "localhost", ntohs(addr.sin_port), 500, callback, &state));
    REQUIRE(request_id != 0);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!done || accepted < 0) && std::chrono::steady_clock::now() < deadline) {
        REQUIRE(event_base_loop(base, EVLOOP_ONCE) == 0);
    }

    REQUIRE(done);
    REQUIRE(callback_result.success);
    REQUIRE(callback_result.type == NTRS_ASYNC_CONNECT_CONTROL);
    REQUIRE(callback_result.control_fd >= 0);
    REQUIRE(accepted >= 0);

    close(callback_result.control_fd);
    close(accepted);
    ntrs_async_client_destroy(client);
    event_free(accept_event);
    event_base_free(base);
    close(listener);
}

TEST_CASE("async detect nat uses event loop udp stun state machine", "[ntrs][client][async]")
{
    struct AsyncNatState {
        event_base*          base;
        bool*                done;
        ntrs_async_result_t* result;
    };

    struct FakeStunServer {
        int response_count;
    };

    event_base*         base = event_base_new();
    int                 server_fd = -1;
    int                 client_fd = -1;
    event*              server_event = NULL;
    sockaddr_in         server_addr;
    socklen_t           server_addr_len = sizeof(server_addr);
    bool                done = false;
    ntrs_async_result_t callback_result;
    FakeStunServer      fake_server = {0};

    REQUIRE(base != NULL);
    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(server_fd >= 0);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(0);
    REQUIRE(bind(server_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == 0);
    REQUIRE(getsockname(server_fd, reinterpret_cast<sockaddr*>(&server_addr), &server_addr_len) == 0);

    client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(client_fd >= 0);

    auto server_cb = [](evutil_socket_t fd, short, void* user_data) {
        FakeStunServer* server = static_cast<FakeStunServer*>(user_data);
        uint8_t         buffer[512];
        sockaddr_in     peer;
        socklen_t       peer_len = sizeof(peer);
        ssize_t nread = recvfrom((int)fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (nread <= 0) {
            return;
        }

        uint8_t  response[32] = {0};
        uint16_t msg_type = htons((uint16_t)eular::stun::StunMsgType::STUN_BINDING_RESPONSE);
        uint16_t msg_len = htons(8);
        uint32_t magic = htonl(0x2112A442u);
        uint16_t attr_type = htons((uint16_t)eular::stun::StunAttributeType::STUN_ATTR_MAPPED_ADDRESS);
        uint16_t attr_len = htons(8);
        uint16_t mapped_port = htons(45678);
        in_addr  mapped_addr;
        inet_pton(AF_INET, "198.51.100.7", &mapped_addr);

        memcpy(response, &msg_type, sizeof(msg_type));
        memcpy(response + 2, &msg_len, sizeof(msg_len));
        memcpy(response + 4, &magic, sizeof(magic));
        if (nread >= 20) {
            memcpy(response + 8, buffer + 8, STUN_TRX_ID_SIZE);
        }
        memcpy(response + 20, &attr_type, sizeof(attr_type));
        memcpy(response + 22, &attr_len, sizeof(attr_len));
        response[25] = 0x01;
        memcpy(response + 26, &mapped_port, sizeof(mapped_port));
        memcpy(response + 28, &mapped_addr.s_addr, sizeof(mapped_addr.s_addr));
        sendto((int)fd, response, sizeof(response), 0, reinterpret_cast<sockaddr*>(&peer), peer_len);
        server->response_count++;
    };

    server_event = event_new(base, server_fd, EV_READ | EV_PERSIST, server_cb, &fake_server);
    REQUIRE(server_event != NULL);
    REQUIRE(event_add(server_event, NULL) == 0);

    ntrs_async_client_t* client = ntrs_async_client_create(base);
    REQUIRE(client != NULL);

    auto callback = [](const ntrs_async_result_t* result, void* user_data) {
        AsyncNatState* state = static_cast<AsyncNatState*>(user_data);
        *state->done = true;
        *state->result = *result;
        event_base_loopbreak(state->base);
    };
    AsyncNatState             state = {base, &done, &callback_result};
    ntrs_detect_nat_options_t options;
    ntrs_detect_nat_options_init(&options);
    options.probe_rounds = 1;
    options.retries_per_round = 1;
    options.enable_filter_probe = false;

    uint64_t request_id = 0;
    REQUIRE(ntrs_async_detect_nat(client, &request_id, client_fd, "localhost", ntohs(server_addr.sin_port), NULL, 0, -1,
                                  "", &options, callback, &state));
    REQUIRE(request_id != 0);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        REQUIRE(event_base_loop(base, EVLOOP_ONCE) == 0);
    }

    REQUIRE(done);
    REQUIRE(callback_result.request_id == request_id);
    REQUIRE(callback_result.type == NTRS_ASYNC_DETECT_NAT);
    INFO(callback_result.error_message);
    INFO(callback_result.nat_info.srflx_ip);
    INFO(callback_result.nat_info.srflx_port);
    INFO(fake_server.response_count);
    REQUIRE(callback_result.success);
    REQUIRE(std::string(callback_result.nat_info.srflx_ip) == "198.51.100.7");
    REQUIRE(callback_result.nat_info.srflx_port == 45678);
    REQUIRE(fake_server.response_count == 1);

    ntrs_async_client_destroy(client);
    event_free(server_event);
    event_base_free(base);
    close(client_fd);
    close(server_fd);
}

TEST_CASE("async udp hole punch selects responding candidate", "[ntrs][client][async]")
{
    struct AsyncPunchState {
        event_base*          base;
        bool*                done;
        ntrs_async_result_t* result;
    };

    event_base*         base = event_base_new();
    int                 local_fd = -1;
    int                 peer_fd = -1;
    event*              peer_event = NULL;
    sockaddr_in         peer_addr;
    socklen_t           peer_addr_len = sizeof(peer_addr);
    bool                done = false;
    ntrs_async_result_t callback_result;

    REQUIRE(base != NULL);
    local_fd = socket(AF_INET, SOCK_DGRAM, 0);
    peer_fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(local_fd >= 0);
    REQUIRE(peer_fd >= 0);

    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer_addr.sin_port = htons(0);
    REQUIRE(bind(peer_fd, reinterpret_cast<sockaddr*>(&peer_addr), sizeof(peer_addr)) == 0);
    REQUIRE(getsockname(peer_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_addr_len) == 0);

    auto peer_cb = [](evutil_socket_t fd, short, void*) {
        uint8_t     buffer[256];
        sockaddr_in src;
        socklen_t   src_len = sizeof(src);
        ssize_t     nread = recvfrom((int)fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&src), &src_len);
        if (nread > 0) {
            sendto((int)fd, buffer, (size_t)nread, 0, reinterpret_cast<sockaddr*>(&src), src_len);
        }
    };
    peer_event = event_new(base, peer_fd, EV_READ | EV_PERSIST, peer_cb, NULL);
    REQUIRE(peer_event != NULL);
    REQUIRE(event_add(peer_event, NULL) == 0);

    ntrs_async_client_t* client = ntrs_async_client_create(base);
    REQUIRE(client != NULL);
    auto callback = [](const ntrs_async_result_t* result, void* user_data) {
        AsyncPunchState* state = static_cast<AsyncPunchState*>(user_data);
        *state->done = true;
        *state->result = *result;
        event_base_loopbreak(state->base);
    };

    ntrs_peer_candidate_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    snprintf(candidate.ip, sizeof(candidate.ip), "%s", "127.0.0.1");
    candidate.port = ntohs(peer_addr.sin_port);
    snprintf(candidate.type, sizeof(candidate.type), "%s", "test");

    AsyncPunchState state = {base, &done, &callback_result};
    uint64_t        request_id = 0;
    REQUIRE(ntrs_async_try_udp_hole_punch(client, &request_id, local_fd, &candidate, 1, 3, 50, callback, &state));
    REQUIRE(request_id != 0);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        REQUIRE(event_base_loop(base, EVLOOP_ONCE) == 0);
    }

    REQUIRE(done);
    REQUIRE(callback_result.success);
    REQUIRE(callback_result.type == NTRS_ASYNC_UDP_HOLE_PUNCH);
    REQUIRE(std::string(callback_result.selected_candidate.ip) == "127.0.0.1");
    REQUIRE(callback_result.selected_candidate.port == ntohs(peer_addr.sin_port));

    ntrs_async_client_destroy(client);
    event_free(peer_event);
    event_base_free(base);
    close(local_fd);
    close(peer_fd);
}
