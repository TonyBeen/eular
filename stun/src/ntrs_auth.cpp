#include <ntrs_auth.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <utils/sysdef.h>

#if defined(OS_WINDOWS)
#include <process.h>
#else
#include <unistd.h>
#endif

#if defined(OS_WINDOWS)
#include <errno.h>
#else
#include <fcntl.h>
#endif
#include <md5.h>
#include <sstream>

#if defined(__linux__)
#include <sys/random.h>
#endif

#if defined(OS_WINDOWS)
static bool fillRandomBytesWindows(void *buf, size_t len)
{
    uint8_t *out = static_cast<uint8_t *>(buf);
    for (size_t i = 0; i < len; ++i) {
        unsigned int value = 0;
        if (rand_s(&value) != 0) {
            return false;
        }
        out[i] = (uint8_t)(value & 0xFFu);
    }
    return true;
}
#endif

namespace eular {
namespace ntrs {

namespace {

static bool fillRandomBytes(void *buf, size_t len)
{
    uint8_t *out = static_cast<uint8_t *>(buf);
    size_t offset = 0;

#if defined(OS_WINDOWS)
    return fillRandomBytesWindows(buf, len);
#endif

#if defined(__linux__)
    while (offset < len) {
        ssize_t n = getrandom(out + offset, len - offset, 0);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        break;
    }
    if (offset == len) {
        return true;
    }
    offset = 0;
#endif

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (offset < len) {
            ssize_t n = read(fd, out + offset, len - offset);
            if (n <= 0) {
                break;
            }
            offset += (size_t)n;
        }
        close(fd);
        if (offset == len) {
            return true;
        }
    }

    return false;
}

static uint64_t weakRandom64()
{
    uint64_t a = (uint64_t)time(NULL);
    uint64_t b = (uint64_t)
#if defined(OS_WINDOWS)
        _getpid();
#else
        getpid();
#endif
    uint64_t c = (uint64_t)rand();
    uint64_t d = (uint64_t)rand();
    return (a << 32) ^ (b << 16) ^ (c << 8) ^ d;
}

static std::string randomHexToken(const char *prefix)
{
    static const char kHex[] = "0123456789abcdef";
    uint8_t bytes[16];
    std::ostringstream oss;

    if (!fillRandomBytes(bytes, sizeof(bytes))) {
        uint64_t value1 = weakRandom64();
        uint64_t value2 = weakRandom64();
        for (size_t i = 0; i < 8; ++i) {
            bytes[i] = (uint8_t)((value1 >> ((7u - i) * 8u)) & 0xFFu);
            bytes[i + 8] = (uint8_t)((value2 >> ((7u - i) * 8u)) & 0xFFu);
        }
    }

    oss << prefix;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        oss << kHex[(bytes[i] >> 4) & 0x0Fu] << kHex[bytes[i] & 0x0Fu];
    }
    return oss.str();
}

static std::string md5Hex(const std::string &input)
{
    static const char kHex[] = "0123456789abcdef";
    MD5_CTX     ctx;
    uint8_t     digest[16];
    std::string out;

    MD5_Init(&ctx);
    MD5_Update(&ctx, input.data(), input.size());
    MD5_Final(digest, &ctx);
    out.reserve(sizeof(digest) * 2u);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[digest[i] & 0x0Fu]);
    }
    return out;
}

}  // namespace

ControlAuthManager::ControlAuthManager(const std::string &shared_secret,
                                       uint32_t session_ttl_sec)
    : shared_secret_(shared_secret.empty() ? "ntrs-dev-secret" : shared_secret)
    , session_ttl_sec_(session_ttl_sec == 0 ? 30 : session_ttl_sec)
{
}

bool ControlAuthManager::issueSession(const std::string &peer_id,
                                      const std::string &bootstrap_token,
                                      int fd,
                                      uint64_t now_sec,
                                      ControlSession *session,
                                      std::string *reason)
{
    if (peer_id.empty()) {
        if (reason != NULL) {
            *reason = "peer_id required";
        }
        return false;
    }
    if (bootstrap_token != shared_secret_) {
        if (reason != NULL) {
            *reason = "invalid bootstrap token";
        }
        return false;
    }

    ControlSession current;
    current.peer_id = peer_id;
    current.token = mintToken(peer_id, fd, now_sec);
    current.fd = fd;
    current.expire_at_sec = now_sec + session_ttl_sec_;
    sessions_[fd] = current;

    if (session != NULL) {
        *session = current;
    }
    if (reason != NULL) {
        reason->clear();
    }
    return true;
}

bool ControlAuthManager::validateSession(int fd,
                                         const std::string &peer_id,
                                         const std::string &session_token,
                                         uint64_t now_sec,
                                         std::string *reason)
{
    std::map<int, ControlSession>::iterator it = sessions_.find(fd);
    if (it == sessions_.end()) {
        if (reason != NULL) {
            *reason = "auth session missing";
        }
        return false;
    }
    if (!peer_id.empty() && it->second.peer_id != peer_id) {
        if (reason != NULL) {
            *reason = "peer_id mismatch";
        }
        return false;
    }
    if (session_token.empty() || it->second.token != session_token) {
        if (reason != NULL) {
            *reason = "session token invalid";
        }
        return false;
    }
    if (now_sec >= it->second.expire_at_sec) {
        if (reason != NULL) {
            *reason = "session token expired";
        }
        return false;
    }

    it->second.expire_at_sec = now_sec + session_ttl_sec_;

    if (reason != NULL) {
        reason->clear();
    }
    return true;
}

bool ControlAuthManager::issuePeerSession(const std::string &src_peer_id,
                                          const std::string &dst_peer_id,
                                          const std::string &session_id,
                                          uint64_t now_sec,
                                          uint32_t ttl_sec,
                                          PeerSessionLease *session,
                                          std::string *reason)
{
    PeerSessionLease current;

    if (src_peer_id.empty() || dst_peer_id.empty() || session_id.empty()) {
        if (reason != NULL) {
            *reason = "session scope required";
        }
        return false;
    }

    current.session_id = session_id;
    current.token = mintPeerSessionToken(src_peer_id, dst_peer_id, now_sec);
    current.src_peer_id = src_peer_id;
    current.dst_peer_id = dst_peer_id;
    current.expire_at_sec = now_sec + (ttl_sec == 0 ? session_ttl_sec_ : ttl_sec);
    peer_sessions_[session_id] = current;

    if (session != NULL) {
        *session = current;
    }
    if (reason != NULL) {
        reason->clear();
    }
    return true;
}

bool ControlAuthManager::validatePeerSession(const std::string &session_id,
                                             const std::string &src_peer_id,
                                             const std::string &dst_peer_id,
                                             const std::string &token,
                                             uint64_t now_sec,
                                             std::string *reason) const
{
    std::map<std::string, PeerSessionLease>::const_iterator it = peer_sessions_.find(session_id);
    if (it == peer_sessions_.end()) {
        if (reason != NULL) {
            *reason = "peer session missing";
        }
        return false;
    }
    if (!src_peer_id.empty() && it->second.src_peer_id != src_peer_id) {
        if (reason != NULL) {
            *reason = "peer session src mismatch";
        }
        return false;
    }
    if (!dst_peer_id.empty() && it->second.dst_peer_id != dst_peer_id) {
        if (reason != NULL) {
            *reason = "peer session dst mismatch";
        }
        return false;
    }
    if (token.empty() || it->second.token != token) {
        if (reason != NULL) {
            *reason = "peer session token invalid";
        }
        return false;
    }
    if (now_sec >= it->second.expire_at_sec) {
        if (reason != NULL) {
            *reason = "peer session token expired";
        }
        return false;
    }

    if (reason != NULL) {
        reason->clear();
    }
    return true;
}

bool ControlAuthManager::sessionPeerId(int fd, std::string *peer_id) const
{
    std::map<int, ControlSession>::const_iterator it = sessions_.find(fd);
    if (it == sessions_.end() || peer_id == NULL) {
        return false;
    }
    *peer_id = it->second.peer_id;
    return !peer_id->empty();
}

void ControlAuthManager::revokeFd(int fd)
{
    sessions_.erase(fd);
}

void ControlAuthManager::revokePeerSession(const std::string &session_id)
{
    peer_sessions_.erase(session_id);
}

void ControlAuthManager::sweepExpired(uint64_t now_sec)
{
    std::map<int, ControlSession>::iterator it = sessions_.begin();
    while (it != sessions_.end()) {
        if (now_sec >= it->second.expire_at_sec) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }

    std::map<std::string, PeerSessionLease>::iterator pit = peer_sessions_.begin();
    while (pit != peer_sessions_.end()) {
        if (now_sec >= pit->second.expire_at_sec) {
            pit = peer_sessions_.erase(pit);
        } else {
            ++pit;
        }
    }
}

uint32_t ControlAuthManager::sessionTtlSec() const
{
    return session_ttl_sec_;
}

std::string ControlAuthManager::mintToken(const std::string &peer_id,
                                          int fd,
                                          uint64_t now_sec) const
{
    (void)peer_id;
    (void)fd;
    (void)now_sec;
    return randomHexToken("ctl_");
}

std::string mintPeerSessionId(const std::string &src_peer_id,
                              const std::string &dst_peer_id,
                              uint64_t now_sec)
{
    (void)src_peer_id;
    (void)dst_peer_id;
    (void)now_sec;
    return randomHexToken("sid_");
}

std::string mintPeerSessionToken(const std::string &src_peer_id,
                                 const std::string &dst_peer_id,
                                 uint64_t now_sec)
{
    (void)src_peer_id;
    (void)dst_peer_id;
    (void)now_sec;
    return randomHexToken("peer_");
}

std::string mintProbeAuthorization(const std::string &shared_secret,
                                   const std::string &owner_peer_id,
                                   const std::string &target_ip,
                                   uint16_t target_port,
                                   const std::string &probe_token,
                                   uint64_t expire_at_sec)
{
    std::ostringstream oss;
    oss << (shared_secret.empty() ? "ntrs-dev-secret" : shared_secret)
        << "|probe|" << owner_peer_id
        << "|" << target_ip
        << "|" << target_port
        << "|" << probe_token
        << "|" << expire_at_sec;
    return md5Hex(oss.str());
}

bool validateProbeAuthorization(const std::string &shared_secret,
                                const std::string &owner_peer_id,
                                const std::string &target_ip,
                                uint16_t target_port,
                                const std::string &probe_token,
                                uint64_t expire_at_sec,
                                const std::string &authorization)
{
    if (owner_peer_id.empty() || target_ip.empty() || target_port == 0 || probe_token.empty() ||
        authorization.empty()) {
        return false;
    }
    return authorization ==
           mintProbeAuthorization(shared_secret, owner_peer_id, target_ip, target_port, probe_token, expire_at_sec);
}

}  // namespace ntrs
}  // namespace eular
