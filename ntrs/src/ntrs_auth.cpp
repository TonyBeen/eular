#include <ntrs/auth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sha256.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <random>
#include <sstream>

#if defined(__linux__)
#include <sys/random.h>
#endif

namespace eular {
namespace ntrs {

namespace {

static std::string HexBytes(const uint8_t* bytes, size_t len)
{
    static const char kHex[] = "0123456789abcdef";
    std::string       out;
    out.reserve(len * 2u);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[bytes[i] & 0x0Fu]);
    }
    return out;
}

static std::string HmacSha256Hex(const std::string& key, const std::string& message)
{
    uint8_t    key_block[SHA256_BLOCK_LENGTH];
    uint8_t    inner_pad[SHA256_BLOCK_LENGTH];
    uint8_t    outer_pad[SHA256_BLOCK_LENGTH];
    uint8_t    key_hash[SHA256_DIGEST_LENGTH];
    uint8_t    inner_hash[SHA256_DIGEST_LENGTH];
    uint8_t    digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;

    memset(key_block, 0, sizeof(key_block));
    if (key.size() > sizeof(key_block)) {
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, key.data(), key.size());
        SHA256_Final(key_hash, &ctx);
        memcpy(key_block, key_hash, sizeof(key_hash));
    } else if (!key.empty()) {
        memcpy(key_block, key.data(), key.size());
    }

    for (size_t i = 0; i < sizeof(key_block); ++i) {
        inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36u);
        outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5cu);
    }

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, inner_pad, sizeof(inner_pad));
    SHA256_Update(&ctx, message.data(), message.size());
    SHA256_Final(inner_hash, &ctx);

    SHA256_Init(&ctx);
    SHA256_Update(&ctx, outer_pad, sizeof(outer_pad));
    SHA256_Update(&ctx, inner_hash, sizeof(inner_hash));
    SHA256_Final(digest, &ctx);
    return HexBytes(digest, sizeof(digest));
}

static bool FillRandomBytes(void* buf, size_t len)
{
    uint8_t* out = static_cast<uint8_t*>(buf);

    try {
        // 优先使用 random_device 获取系统熵源，避免使用可预测的统计型伪随机序列；
        // 构造 random_device 可能有系统开销，因此每个线程复用一个实例。
        static thread_local std::random_device rd;
        size_t                                 offset = 0;
        while (offset < len) {
            unsigned int value = rd();
            for (size_t i = 0; i < sizeof(value) && offset < len; ++i) {
                out[offset++] = static_cast<uint8_t>((value >> (i * 8u)) & 0xFFu);
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

static bool FillRandomBytesPlatform(void* buf, size_t len)
{
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t   offset = 0;

#if defined(_WIN32)
    while (offset < len) {
        unsigned int value = 0;
        if (rand_s(&value) != 0) {
            return false;
        }
        for (size_t i = 0; i < sizeof(value) && offset < len; ++i) {
            out[offset++] = static_cast<uint8_t>((value >> (i * 8u)) & 0xFFu);
        }
    }
    return true;
#else
#if defined(__linux__)
    while (offset < len) {
        ssize_t n = getrandom(out + offset, len - offset, 0);
        if (n > 0) {
            offset += static_cast<size_t>(n);
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
    if (fd < 0) {
        return false;
    }
    while (offset < len) {
        ssize_t n = read(fd, out + offset, len - offset);
        if (n <= 0) {
            break;
        }
        offset += static_cast<size_t>(n);
    }
    close(fd);
    return offset == len;
#endif
}

static uint64_t WeakRandom64()
{
    uint64_t a = static_cast<uint64_t>(time(NULL));
    uint64_t b =
#if defined(_WIN32)
        static_cast<uint64_t>(_getpid());
#else
        static_cast<uint64_t>(getpid());
#endif
    uint64_t c = static_cast<uint64_t>(rand());
    uint64_t d = static_cast<uint64_t>(rand());
    return (a << 32) ^ (b << 16) ^ (c << 8) ^ d;
}

static void FillRandomBytesWeak(void* buf, size_t len)
{
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t   offset = 0;

    while (offset < len) {
        uint64_t value = WeakRandom64();
        for (size_t i = 0; i < sizeof(value) && offset < len; ++i) {
            out[offset++] = static_cast<uint8_t>((value >> ((7u - i) * 8u)) & 0xFFu);
        }
    }
}

static std::string RandomHexToken(const char* prefix)
{
    static const char  kHex[] = "0123456789abcdef";
    uint8_t            bytes[16];
    std::ostringstream oss;

    if (!FillRandomBytes(bytes, sizeof(bytes)) && !FillRandomBytesPlatform(bytes, sizeof(bytes))) {
        // 极端情况下安全随机源不可用时，最后使用弱随机兜底，保证探测流程可继续。
        FillRandomBytesWeak(bytes, sizeof(bytes));
    }

    oss << prefix;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        oss << kHex[(bytes[i] >> 4) & 0x0Fu] << kHex[bytes[i] & 0x0Fu];
    }
    return oss.str();
}

}  // namespace

ControlAuthManager::ControlAuthManager(const std::string& shared_secret, uint32_t session_ttl_sec)
    : shared_secret_(shared_secret.empty() ? "ntrs-dev-secret" : shared_secret),
      session_ttl_sec_(session_ttl_sec == 0 ? 30 : session_ttl_sec)
{
}

bool ControlAuthManager::issueSession(const std::string& peer_id, const std::string& bootstrap_token, int fd,
                                      uint64_t now_sec, ControlSession* session, std::string* reason)
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
    if (current.token.empty()) {
        if (reason != NULL) {
            *reason = "token random source unavailable";
        }
        return false;
    }
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

bool ControlAuthManager::validateSession(int fd, const std::string& peer_id, const std::string& session_token,
                                         uint64_t now_sec, std::string* reason)
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

bool ControlAuthManager::issuePeerSession(const std::string& src_peer_id, const std::string& dst_peer_id,
                                          const std::string& session_id, uint64_t now_sec, uint32_t ttl_sec,
                                          PeerSessionLease* session, std::string* reason)
{
    PeerSessionLease current;

    if (src_peer_id.empty() || dst_peer_id.empty() || session_id.empty()) {
        if (reason != NULL) {
            *reason = "session scope required";
        }
        return false;
    }

    current.session_id = session_id;
    current.token = MintPeerSessionToken(src_peer_id, dst_peer_id, now_sec);
    if (current.token.empty()) {
        if (reason != NULL) {
            *reason = "token random source unavailable";
        }
        return false;
    }
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

bool ControlAuthManager::validatePeerSession(const std::string& session_id, const std::string& src_peer_id,
                                             const std::string& dst_peer_id, const std::string& token, uint64_t now_sec,
                                             std::string* reason) const
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

bool ControlAuthManager::sessionPeerId(int fd, std::string* peer_id) const
{
    std::map<int, ControlSession>::const_iterator it = sessions_.find(fd);
    if (it == sessions_.end() || peer_id == NULL) {
        return false;
    }
    *peer_id = it->second.peer_id;
    return !peer_id->empty();
}

void ControlAuthManager::revokeFd(int fd) { sessions_.erase(fd); }

void ControlAuthManager::revokePeerSession(const std::string& session_id) { peer_sessions_.erase(session_id); }

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

uint32_t ControlAuthManager::sessionTtlSec() const { return session_ttl_sec_; }

std::string ControlAuthManager::mintToken(const std::string& peer_id, int fd, uint64_t now_sec) const
{
    (void)peer_id;
    (void)fd;
    (void)now_sec;
    return RandomHexToken("ctl_");
}

std::string MintPeerSessionId(const std::string& src_peer_id, const std::string& dst_peer_id, uint64_t now_sec)
{
    (void)src_peer_id;
    (void)dst_peer_id;
    (void)now_sec;
    return RandomHexToken("sid_");
}

std::string MintPeerSessionToken(const std::string& src_peer_id, const std::string& dst_peer_id, uint64_t now_sec)
{
    (void)src_peer_id;
    (void)dst_peer_id;
    (void)now_sec;
    return RandomHexToken("peer_");
}

std::string MintProbeAuthorization(const std::string& shared_secret, const std::string& owner_peer_id,
                                   const std::string& target_ip, uint16_t target_port, const std::string& probe_token,
                                   uint64_t expire_at_sec)
{
    std::ostringstream oss;
    if (shared_secret.empty()) {
        return "";
    }
    oss << "probe|" << owner_peer_id << "|" << target_ip << "|" << target_port << "|" << probe_token << "|"
        << expire_at_sec;
    return HmacSha256Hex(shared_secret, oss.str());
}

bool ValidateProbeAuthorization(const std::string& shared_secret, const std::string& owner_peer_id,
                                const std::string& target_ip, uint16_t target_port, const std::string& probe_token,
                                uint64_t expire_at_sec, const std::string& authorization)
{
    if (owner_peer_id.empty() || target_ip.empty() || target_port == 0 || probe_token.empty() ||
        authorization.empty()) {
        return false;
    }
    return authorization ==
           MintProbeAuthorization(shared_secret, owner_peer_id, target_ip, target_port, probe_token, expire_at_sec);
}

}  // namespace ntrs
}  // namespace eular
