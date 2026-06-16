#ifndef __NTRS_HUB_STATE_H__
#define __NTRS_HUB_STATE_H__

#include <ntrs_codec.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace eular {
namespace ntrs {

struct ClusterNodeState {
    std::string node_id;
    std::string boot_id;
    std::string status;
    std::string region;
    /**
     * @brief 节点对外暴露的私有 NAT 探测端点。
     *
     * 该字段表示控制面下发给客户端的私有探测入口。
     */
    std::string probe_endpoint;
    std::string control_endpoint;
    std::string nat_type;
    std::string last_heartbeat;
    uint32_t    heartbeat_interval_sec;
    uint64_t    last_seen_mono_sec;
    int32_t     load;
};

class HubClusterState
{
public:
    HubClusterState();

    bool applyMessage(const std::string& topic_node_id, const Message& msg, uint64_t now_mono_sec,
                      const std::string& now_iso8601, std::string* event_name, ClusterNodeState* event_node);

    bool sweepExpired(uint64_t now_mono_sec, std::vector<ClusterNodeState>* evicted_nodes);
    bool restoreFromSnapshot(const Message& msg);

    const std::map<std::string, ClusterNodeState>& nodes() const;
    uint64_t                                       cluster_version() const;

private:
    bool     shouldReplaceGeneration(const ClusterNodeState& current, const std::string& incoming_boot_id) const;
    bool     isCurrentGeneration(const ClusterNodeState& current, const std::string& incoming_boot_id) const;
    uint64_t bootGenerationOrder(const std::string& boot_id) const;
    uint32_t heartbeatTimeoutSec(const ClusterNodeState& node) const;
    void     bumpVersion();

    std::map<std::string, ClusterNodeState> nodes_;
    uint64_t                                cluster_version_;
};

bool encodeClusterSnapshotNodes(const std::map<std::string, ClusterNodeState>& nodes, std::vector<uint8_t>* out);
bool decodeClusterSnapshotNodes(const void* data, size_t len, std::vector<ClusterNodeState>* nodes);
bool buildClusterSnapshotMessage(const std::map<std::string, ClusterNodeState>& nodes,
                                 uint64_t                                       cluster_version,
                                 const std::string&                             ts,
                                 Message*                                       msg);
bool parseClusterSnapshotMessage(const Message& msg, uint64_t* cluster_version, std::vector<ClusterNodeState>* nodes);

}  // namespace ntrs
}  // namespace eular

#endif
