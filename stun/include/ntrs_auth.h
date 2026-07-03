#ifndef __NTRS_AUTH_H__
#define __NTRS_AUTH_H__

#include <stdint.h>

#include <map>
#include <string>

namespace eular {
namespace ntrs {

struct ControlSession {
    std::string peer_id;
    std::string token;
    int         fd;
    uint64_t    expire_at_sec;
};

struct PeerSessionLease {
    std::string session_id;
    std::string token;
    std::string src_peer_id;
    std::string dst_peer_id;
    uint64_t    expire_at_sec;
};

class ControlAuthManager
{
public:
    ControlAuthManager(const std::string& shared_secret, uint32_t session_ttl_sec);

    bool issueSession(const std::string& peer_id, const std::string& bootstrap_token, int fd, uint64_t now_sec,
                      ControlSession* session, std::string* reason);

    bool validateSession(int fd, const std::string& peer_id, const std::string& session_token, uint64_t now_sec,
                         std::string* reason);

    bool issuePeerSession(const std::string& src_peer_id, const std::string& dst_peer_id,
                          const std::string& session_id, uint64_t now_sec, uint32_t ttl_sec,
                          PeerSessionLease* session, std::string* reason);

    bool validatePeerSession(const std::string& session_id, const std::string& src_peer_id,
                             const std::string& dst_peer_id, const std::string& token,
                             uint64_t now_sec, std::string* reason) const;

    bool sessionPeerId(int fd, std::string* peer_id) const;

    void     revokeFd(int fd);
    void     revokePeerSession(const std::string& session_id);
    void     sweepExpired(uint64_t now_sec);
    uint32_t sessionTtlSec() const;

private:
    std::string mintToken(const std::string& peer_id, int fd, uint64_t now_sec) const;

    std::string                             shared_secret_;
    uint32_t                                session_ttl_sec_;
    std::map<int, ControlSession>           sessions_;
    std::map<std::string, PeerSessionLease> peer_sessions_;
};

std::string mintPeerSessionId(const std::string& src_peer_id, const std::string& dst_peer_id, uint64_t now_sec);
std::string mintPeerSessionToken(const std::string& src_peer_id, const std::string& dst_peer_id, uint64_t now_sec);
std::string mintProbeAuthorization(const std::string& shared_secret,
                                   const std::string& owner_peer_id,
                                   const std::string& target_ip,
                                   uint16_t           target_port,
                                   const std::string& probe_token,
                                   uint64_t           expire_at_sec);
bool        validateProbeAuthorization(const std::string& shared_secret,
                                       const std::string& owner_peer_id,
                                       const std::string& target_ip,
                                       uint16_t           target_port,
                                       const std::string& probe_token,
                                       uint64_t           expire_at_sec,
                                       const std::string& authorization);

}  // namespace ntrs
}  // namespace eular

#endif
